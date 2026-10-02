#include <string.h>
#include "bookmarks.h"
#include "platform/m65_far.h"

#ifdef __MEGA65__
#include "platform/m65_cbmdos.h"
/* The load buffers alias the transfer scratch at $1400/$1500 (xmodem.c
 * file_buf, zmodem.c cur_buf/next_buf): a transfer never runs while the
 * dialing directory is loading. Keeping them off the stack keeps main's
 * frame small; as locals they made it 416 bytes, which overlapped .bss. */
#define LOAD_BUF  ((unsigned char *)0x1400)
#define LOAD_LINE ((char *)0x1500)
#else
#include <stdio.h>
#endif

#define LINE_MAX 128

const unsigned int speed_baud[5] = { 300, 1200, 2400, 9600, 0 };

/* The table lives in bank 1 ($11800, 16 x 69 bytes), outside the CPU's
 * map; bm_win is the one near copy. bookmarks_get returns a pointer to
 * it, valid until the next bookmarks_ call. */
#define BMS_FAR (FAR_BASE + 0xC00UL)   /* $12400, past the transfer buffer: 200 x 69 bytes to $159E7 (2026-10-02) */
/* Offsets are computed in 16 bits: a 32-bit multiply costs a library
 * call at every accessor. */
#define BM_OFF(i) ((unsigned int)(i) * (unsigned int)sizeof(bookmark_t))
static bookmark_t bm_win;
static unsigned char bm_count = 0;

/* The starting table when there is no BBSCFG, in the file's own format
 * (name|host|port|emul|res; emul 0 PETSCII 1 ANSI; res 0 40x25, 1 80x25,
 * 2 80x50) so one parser serves both. */
static const char defaults_txt[] =
  "Retrocampus|bbs.retrocampus.com|6510|0|0\n"
  "Particles BBS|particlesbbs.dyndns.org|6400|0|0\n"
  "Borderline BBS|bbs.borderline.org|23|0|1\n"
  "Heatwave BBS|heatwave.ddns.net|9640|0|0\n"
  "Black Flag BBS|blackflag.acid.org|23|1|1\n"
  "Synchronet BBS|vert.synchro.net|23|1|1\n"
  "ConChaos BBS|conchaos.synchro.net|23|1|1\n";

static __attribute__((noinline)) void bm_read(unsigned char i, bookmark_t *dst)
{
  far_read(BMS_FAR + BM_OFF(i), dst, sizeof(bookmark_t));
}

static __attribute__((noinline)) void bm_write(unsigned char i, const bookmark_t *src)
{
  far_write(src, BMS_FAR + BM_OFF(i), sizeof(bookmark_t));
}

unsigned char bookmarks_count(void)
{
  return bm_count;
}

bookmark_t *bookmarks_get(unsigned char index)
{
  if (index >= bm_count) return 0;
  bm_read(index, &bm_win);
  return &bm_win;
}

unsigned char bookmarks_add(const bookmark_t *bm)
{
  if (bm_count >= BOOKMARK_MAX) return 0;
  bm_write(bm_count, bm);
  bm_count++;
  return 1;
}

unsigned char bookmarks_update(unsigned char index, const bookmark_t *bm)
{
  if (index >= bm_count) return 0;
  bm_write(index, bm);
  return 1;
}

unsigned char bookmarks_delete(unsigned char index)
{
  unsigned char tail;
  if (index >= bm_count) return 0;
  tail = (unsigned char)(bm_count - 1 - index);
  if (tail)                                   /* zero-length DMA copies 64KB */
    far_move(BMS_FAR + BM_OFF(index + 1), BMS_FAR + BM_OFF(index), BM_OFF(tail));
  bm_count--;
  return 1;
}

/* name|host|port|emul|res|speed, the last three optional; the speed is
 * a baud rate, 0 for unlimited, and anything else (older files kept a
 * drive number there) reads as unlimited. Parses into the window and
 * appends it. */
static void parse_line(char *line)
{
  char *p = line;
  char *field;
  unsigned int port = 0;

  memset(&bm_win, 0, sizeof(bm_win));
  bm_win.speed = SPEED_MAX;

  /* Field 1: Name */
  field = p;
  while (*p && *p != '|') p++;
  if (!*p) return;
  *p++ = 0;
  strncpy(bm_win.name, field, sizeof(bm_win.name) - 1);

  /* Field 2: Host */
  field = p;
  while (*p && *p != '|') p++;
  if (!*p) return;
  *p++ = 0;
  strncpy(bm_win.host, field, sizeof(bm_win.host) - 1);

  /* Field 3: Port */
  field = p;
  while (*p && *p != '|') p++;
  if (!*p) return;
  *p++ = 0;
  while (*field >= '0' && *field <= '9') {
    port = port * 10 + (unsigned int)(*field - '0');
    field++;
  }
  bm_win.port = port ? port : 23;

  /* Field 4: Emul */
  if (*p >= '0' && *p <= '1') bm_win.emul = (unsigned char)(*p - '0');
  while (*p && *p != '|') p++;
  if (!*p) { bookmarks_add(&bm_win); return; }
  p++;

  /* Field 5: Res */
  if (*p >= '0' && *p <= '3') bm_win.res = (unsigned char)(*p - '0');
  while (*p && *p != '|') p++;
  if (!*p) { bookmarks_add(&bm_win); return; }
  p++;

  /* Field 6: Speed, as a baud rate */
  port = 0;
  while (*p >= '0' && *p <= '9') { port = port * 10 + (unsigned int)(*p - '0'); p++; }
  {
    unsigned char i;
    for (i = 0; i < 4; i++) if (port == speed_baud[i]) bm_win.speed = i;
  }

  bookmarks_add(&bm_win);
}

unsigned char bookmarks_load(unsigned char drive)
{
  bm_count = 0;
#ifdef __MEGA65__
  {
    unsigned char err;
    unsigned int n;
    unsigned char *buf = LOAD_BUF;
    char *line = LOAD_LINE;
    unsigned char line_pos = 0;

    err = cbmdos_open_read("BBSCFG", drive);
    if (err != CBMDOS_OK) return 0;

    while ((n = cbmdos_read_next(buf, &err)) > 0) {
      unsigned int i;
      for (i = 0; i < n; i++) {
        unsigned char c = buf[i];
        if (c == '\n' || c == '\r') {
          if (line_pos > 0) {
            line[line_pos] = 0;
            parse_line(line);
            line_pos = 0;
          }
        } else if (line_pos < LINE_MAX - 1) {
          line[line_pos++] = (char)c;
        }
      }
    }
    if (line_pos > 0) {
      line[line_pos] = 0;
      parse_line(line);
    }
    cbmdos_close_read();
    return (bm_count > 0);
  }
#else
  {
    (void)drive;
    FILE *f = fopen("bbscfg.txt", "r");
    char line[LINE_MAX];
    if (!f) return 0;
    while (fgets(line, sizeof(line), f)) {
      char *nl = strchr(line, '\n');
      if (nl) *nl = 0;
      nl = strchr(line, '\r');
      if (nl) *nl = 0;
      if (line[0]) parse_line(line);
    }
    fclose(f);
    return (bm_count > 0);
  }
#endif
}

#ifdef __MEGA65__
static unsigned char put_str(const char *s)
{
  while (*s)
    if (cbmdos_put((unsigned char)*s++) != CBMDOS_OK) return 0;
  return 1;
}

static unsigned char put_uint(unsigned int val)
{
  char buf[6];
  unsigned char i = 0;
  if (val == 0) return put_str("0");
  while (val > 0) {
    buf[i++] = (char)('0' + (val % 10));
    val /= 10;
  }
  while (i > 0)
    if (cbmdos_put((unsigned char)buf[--i]) != CBMDOS_OK) return 0;
  return 1;
}
#endif

/* Rewrites BBSCFG on `drive`. Returns 0 if the disk could not be written;
 * the caller tells the user, because a failed save on a drive with no disk
 * takes about twenty seconds of retries and otherwise looks like a hang. */
unsigned char bookmarks_save(unsigned char drive)
{
#ifdef __MEGA65__
  unsigned char i, err;
  err = cbmdos_delete("BBSCFG", drive);
  if (err != CBMDOS_OK && err != CBMDOS_ERR_NOTFOUND) return 0;
  if (cbmdos_create("BBSCFG", drive) != CBMDOS_OK) return 0;

  for (i = 0; i < bm_count; i++) {
    bm_read(i, &bm_win);
    if (!put_str(bm_win.name) || !put_str("|") ||
        !put_str(bm_win.host) || !put_str("|") ||
        !put_uint(bm_win.port) || !put_str("|") ||
        !put_uint(bm_win.emul) || !put_str("|") ||
        !put_uint(bm_win.res) || !put_str("|") ||
        !put_uint(speed_baud[bm_win.speed > 4 ? 4 : bm_win.speed]) || !put_str("\n")) {
      cbmdos_close();
      return 0;
    }
  }
  return cbmdos_close() == CBMDOS_OK;
#else
  {
    (void)drive;
    unsigned char i;
    FILE *f = fopen("bbscfg.txt", "w");
    if (!f) return 0;
    for (i = 0; i < bm_count; i++) {
      bm_read(i, &bm_win);
      fprintf(f, "%s|%s|%u|%u|%u|%u\n",
              bm_win.name, bm_win.host, bm_win.port,
              bm_win.emul, bm_win.res, speed_baud[bm_win.speed > 4 ? 4 : bm_win.speed]);
    }
    fclose(f);
    return 1;
  }
#endif
}

void bookmarks_init(unsigned char boot_drive)
{
  if (!bookmarks_load(boot_drive)) {
    const char *p = defaults_txt;
#ifdef __MEGA65__
    char *line = LOAD_LINE;
#else
    char line[LINE_MAX];
#endif
    bm_count = 0;
    while (*p) {
      unsigned char n = 0;
      while (*p && *p != '\n' && n < LINE_MAX - 1) line[n++] = *p++;
      line[n] = 0;
      if (*p) p++;
      parse_line(line);
    }
    bookmarks_save(boot_drive);
  }
}
