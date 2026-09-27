/* ZMODEM over the telnet stream: receive (started by the sender's ZRQINIT
 * or from the transfer menu) and send.
 *
 * What a real sender or receiver (lrzsz, Synchronet) needs that the first
 * version lacked: every data subpacket's CRC checked and a ZRPOS sent when
 * it fails; 0xFF escaped as ZDLE 'm', which lrzsz requires; every send
 * completed through the stack's 3 KB buffer; only the sender saying "OO".
 * Data is held in bank 1 until its CRC is known, because a block already
 * written to disk cannot be taken back.
 *
 * CRC-32 is not offered in ZRINIT and not accepted: a sender only uses it
 * when the receiver asks, and the bitwise form costs code and time. */
#include <string.h>
#include "zmodem.h"
#include "netutil.h"
#include "telnet.h"
#include "ui.h"
#include "platform/m65_cbmdos.h"
#include "platform/m65_far.h"

#ifdef __MEGA65__
#include "mega65/memory.h"
#else
#define PEEK(addr) 0
#endif

/* Framing characters */
#define ZPAD  '*'
#define ZDLE  0x18
#define ZBIN  'A'
#define ZHEX  'B'
#define ZBIN32 'C'

#define ZCRCE 'h'
#define ZCRCG 'i'
#define ZCRCQ 'j'
#define ZCRCW 'k'
#define ZRUB0 'l'
#define ZRUB1 'm'

/* Frame types */
#define ZRQINIT   0
#define ZRINIT    1
#define ZSINIT    2
#define ZACK      3
#define ZFILE     4
#define ZSKIP     5
#define ZNAK      6
#define ZABORT    7
#define ZFIN      8
#define ZRPOS     9
#define ZDATA     10
#define ZEOF      11

/* ZRINIT: a 1024-byte subpacket limit in ZP0/ZP1 (the bank-1 buffer), and
 * CANFDX | CANOVIO in ZF0. */
#define ZRINIT_FLAGS 0x03000400UL

#define XFER_FAR 0x12000UL      /* one subpacket, until its CRC is known */
#define XFER_MAX 1024u

#define TIMEOUT_FRAMES 1200     /* ~20 seconds within a frame */
#define HEADER_FRAMES  600      /* ~10 seconds for the next header */
#define MAX_ERRORS     10

/* rx_subpacket / rx_escaped_byte results */
#define RX_TIMEOUT 0
#define RX_DATA    1
#define RX_DELIM   2
#define RX_BAD     3
#define RX_CANCEL  4

static zmodem_status_t cur_status;
static zmodem_progress_fn progress_cb = 0;
static unsigned char link_ok, user_abort, cancel;
static unsigned char unget_byte, unget_have;

static const char hex_digits[] = "0123456789abcdef";

/* CAN x8 and backspaces: the abort string every ZMODEM recognises. */
static const unsigned char canistr[] = {
  0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 0x18, 8, 8, 8, 8, 8, 8, 8, 8
};

#ifdef __MEGA65__
#define cur_buf ((unsigned char *)0x1400)
#define next_buf ((unsigned char *)0x1500)
#else
static unsigned char cur_buf[256];
static unsigned char next_buf[256];
#endif

/* ---- CRCs ------------------------------------------------------------- */

/* CRC-16/XMODEM, direct form: the same value the old 512-byte table gave
 * after its two trailing zero feeds. Data followed by its CRC comes to 0. */
static __attribute__((noinline)) unsigned int z_crc16(unsigned int crc, unsigned char b)
{
  unsigned char i;
  crc ^= (unsigned int)b << 8;
  for (i = 0; i < 8; i++)
    crc = (crc & 0x8000) ? (unsigned int)((crc << 1) ^ 0x1021) : (unsigned int)(crc << 1);
  return crc & 0xffff;                          /* the host's int is wider than 16 bits */
}

/* ---- sending ---------------------------------------------------------- */

static void tx_raw(const unsigned char *data, unsigned int len)
{
  if (link_ok && !telnet_tx_data(data, len)) link_ok = 0;
}

static void tx_byte(unsigned char b)
{
  tx_raw(&b, 1);
}

/* ZDLE escaping as lrzsz does it: 0xFF and 0x7F as ZRUB1/ZRUB0, the
 * control characters XORed with 0x40. Anything else after ZDLE is refused
 * by a real receiver. */
static void tx_escaped(unsigned char b)
{
  unsigned char seq[2];
  seq[0] = ZDLE;
  if (b == 0xff) seq[1] = ZRUB1;
  else if (b == 0x7f) seq[1] = ZRUB0;
  else if (b == ZDLE || b == 0x11 || b == 0x13 || b == 0x91 || b == 0x93) seq[1] = (unsigned char)(b ^ 0x40);
  else { tx_raw(&b, 1); return; }
  tx_raw(seq, 2);
}

static void send_hex_header(unsigned char type, unsigned long flags)
{
  unsigned char buf[24];
  unsigned char len = 0;
  unsigned int crc;
  unsigned char f[4];
  unsigned char i;

  f[0] = (unsigned char)(flags & 0xff);
  f[1] = (unsigned char)((flags >> 8) & 0xff);
  f[2] = (unsigned char)((flags >> 16) & 0xff);
  f[3] = (unsigned char)((flags >> 24) & 0xff);

  crc = z_crc16(0, type);
  for (i = 0; i < 4; i++) crc = z_crc16(crc, f[i]);

  buf[len++] = ZPAD;
  buf[len++] = ZPAD;
  buf[len++] = ZDLE;
  buf[len++] = ZHEX;

  buf[len++] = (unsigned char)hex_digits[(type >> 4) & 0x0f];
  buf[len++] = (unsigned char)hex_digits[type & 0x0f];

  for (i = 0; i < 4; i++) {
    buf[len++] = (unsigned char)hex_digits[(f[i] >> 4) & 0x0f];
    buf[len++] = (unsigned char)hex_digits[f[i] & 0x0f];
  }

  buf[len++] = (unsigned char)hex_digits[(crc >> 12) & 0x0f];
  buf[len++] = (unsigned char)hex_digits[(crc >> 8) & 0x0f];
  buf[len++] = (unsigned char)hex_digits[(crc >> 4) & 0x0f];
  buf[len++] = (unsigned char)hex_digits[crc & 0x0f];

  buf[len++] = 015;  /* \r */
  buf[len++] = 0212; /* \x8a */
  if (type != ZACK && type != ZFIN) {
    buf[len++] = 021; /* XON */
  }

  tx_raw(buf, len);
}

/* ---- receiving -------------------------------------------------------- */

static void unrx_raw_byte(unsigned char b)
{
  unget_byte = b;
  unget_have = 1;
}

/* One stream byte, flow-control characters dropped, within timeout_frames.
 * Returns 0 on timeout, a dead link, or a keyboard abort (user_abort). */
static unsigned char rx_raw_byte(unsigned char *out, unsigned int timeout_frames)
{
  unsigned int frames = 0;
#ifdef __MEGA65__
  unsigned char last_f = PEEK(0xd7fa);
  unsigned int spin = 0;
  unsigned int wraps = 0;
#endif

  if (unget_have) {
    unget_have = 0;
    *out = unget_byte;
    return 1;
  }

  for (;;) {
    unsigned char b;
    unsigned char k;
    if (telnet_rx_byte(&b)) {
      if (b == 0x11 || b == 0x13 || b == 0x91 || b == 0x93) continue;
      *out = b;
      return 1;
    }
    if (!net_alive()) { link_ok = 0; return 0; }
    net_poll();
    k = ui_key();
    if (k == KEY_STOP || k == KEY_ESC || k == 0x03 || k == 0x18) {
      user_abort = 1;
      return 0;
    }
#ifdef __MEGA65__
    if (PEEK(0xd7fa) != last_f) {
      last_f = PEEK(0xd7fa);
      if (++frames >= timeout_frames) return 0;
    }
    if (++spin == 0) {
      if (++wraps >= 500) return 0;
    }
#else
    if (++frames >= timeout_frames) return 0;
#endif
  }
}

/* One data byte with ZDLE escaping undone: RX_DATA, RX_DELIM (*out is
 * the ZCRCx delimiter), RX_BAD for an escape a real sender never emits,
 * RX_CANCEL for the peer's CAN CAN CAN, RX_TIMEOUT. */
static unsigned char rx_escaped_byte(unsigned char *out, unsigned int timeout_frames)
{
  unsigned char c, cans = 0;
  if (!rx_raw_byte(&c, timeout_frames)) return RX_TIMEOUT;
  if (c != ZDLE) { *out = c; return RX_DATA; }
  for (;;) {
    if (!rx_raw_byte(&c, timeout_frames)) return RX_TIMEOUT;
    if (c != ZDLE) break;
    if (++cans >= 2) return RX_CANCEL;              /* CAN CAN CAN: the peer gave up */
  }
  if (c == ZCRCE || c == ZCRCG || c == ZCRCQ || c == ZCRCW) { *out = c; return RX_DELIM; }
  if (c == ZRUB0) { *out = 0x7f; return RX_DATA; }
  if (c == ZRUB1) { *out = 0xff; return RX_DATA; }
  if ((c & 0x60) == 0x40) { *out = (unsigned char)(c ^ 0x40); return RX_DATA; }
  return RX_BAD;
}

static unsigned char hex_val(unsigned char c)
{
  if (c >= '0' && c <= '9') return (unsigned char)(c - '0');
  if (c >= 'a' && c <= 'f') return (unsigned char)(c - 'a' + 10);
  if (c >= 'A' && c <= 'F') return (unsigned char)(c - 'A' + 10);
  return 0xff;
}

static unsigned char read_hex_byte(unsigned char *out)
{
  unsigned char c1, c2, h1, h2;
  if (!rx_raw_byte(&c1, TIMEOUT_FRAMES)) return 0;
  if (!rx_raw_byte(&c2, TIMEOUT_FRAMES)) return 0;
  h1 = hex_val(c1);
  h2 = hex_val(c2);
  if (h1 > 15 || h2 > 15) return 0;
  *out = (unsigned char)((h1 << 4) | h2);
  return 1;
}

/* Reads the five bytes of a header body in the given format and checks
 * its CRC. Returns 1 with the type and flags. */
static unsigned char rx_header_body(unsigned char fmt, unsigned char *hdr_type, unsigned long *hdr_flags)
{
  unsigned char h[5], i, c;
  unsigned int crc;

  crc = 0;
  for (i = 0; i < 7; i++) {                  /* type, four flag bytes, CRC */
    if (fmt == ZHEX) { if (!read_hex_byte(&c)) return 0; }
    else if (rx_escaped_byte(&c, TIMEOUT_FRAMES) != RX_DATA) return 0;
    if (i < 5) h[i] = c;
    crc = z_crc16(crc, c);
  }
  if (crc != 0) return 0;
  *hdr_type = h[0];
  *hdr_flags = (unsigned long)h[1] | ((unsigned long)h[2] << 8) |
               ((unsigned long)h[3] << 16) | ((unsigned long)h[4] << 24);
  if (fmt == ZHEX) {
    /* The hex trailer: CR, LF (or 0x8a), XON. Anything else is data. */
    while (rx_raw_byte(&c, 2)) {
      if (c != 13 && c != 10 && c != 138) { unrx_raw_byte(c); break; }
    }
  }
  return 1;
}

/* Waits up to max_frames for a header, scanning past anything that is
 * not one (noise, data of a frame that lost its position, a header with
 * a bad CRC). */
static unsigned char rx_header(unsigned char *hdr_type, unsigned long *hdr_flags, unsigned int max_frames)
{
  unsigned char c;

  while (max_frames > 0) {
    unsigned int wait_chunk = (max_frames > 25) ? 25 : max_frames;
    if (!rx_raw_byte(&c, wait_chunk)) {
      if (!link_ok || user_abort) return 0;
      if (max_frames <= wait_chunk) return 0;
      max_frames -= wait_chunk;
      continue;
    }
    if (c != ZPAD) continue;
    for (;;) {                                 /* one or more ZPADs, then ZDLE */
      if (!rx_raw_byte(&c, 50)) break;
      if (c == ZPAD) continue;
      if (c != ZDLE) break;
      if (!rx_raw_byte(&c, 50)) break;
      if (c != ZHEX && c != ZBIN) break;       /* ZBIN32 was not offered */
      if (rx_header_body(c, hdr_type, hdr_flags)) return 1;
      break;                                   /* bad CRC or short: keep scanning */
    }
  }
  return 0;
}

/* One data subpacket into the bank-1 buffer, CRC checked. RX_DATA with
 * the count and delimiter, RX_BAD for a CRC error, an overrun or a bad
 * escape, RX_CANCEL, RX_TIMEOUT. */
static unsigned char rx_subpacket(unsigned int *n_out, unsigned char *delim)
{
  unsigned int n = 0;
  unsigned int crc = 0;
  unsigned char b, res, i;

  for (;;) {
    res = rx_escaped_byte(&b, TIMEOUT_FRAMES);
    if (res == RX_DELIM) break;
    if (res != RX_DATA) return res;
    if (n >= XFER_MAX) return RX_BAD;
    far_poke(XFER_FAR + n, b);
    n++;
    crc = z_crc16(crc, b);
  }
  *delim = b;
  crc = z_crc16(crc, b);
  for (i = 0; i < 2; i++) {
    if (rx_escaped_byte(&b, TIMEOUT_FRAMES) != RX_DATA) return RX_TIMEOUT;
    crc = z_crc16(crc, b);
  }
  if (crc != 0) return RX_BAD;
  *n_out = n;
  return RX_DATA;
}

/* The verified subpacket to disk, 254 bytes at a time through cur_buf. */
static unsigned char flush_far(unsigned int n)
{
  unsigned int off = 0;
  while (off < n) {
    unsigned int k = n - off, i;
    if (k > 254) k = 254;
    far_read(XFER_FAR + off, cur_buf, k);
    for (i = 0; i < k; i++)
      if (cbmdos_put(cur_buf[i]) != CBMDOS_OK) return 0;
    off += k;
  }
  return 1;
}

static void update_progress(void)
{
  if (cur_status.file_size) cur_status.percent = transfer_percent(cur_status.bytes_transferred, cur_status.file_size);
  if (progress_cb) progress_cb(&cur_status);
}

void zmodem_init(zmodem_progress_fn prog_fn)
{
  progress_cb = prog_fn;
  memset(&cur_status, 0, sizeof(cur_status));
}

const zmodem_status_t *zmodem_get_status(void)
{
  return &cur_status;
}

static void begin(unsigned char drive, unsigned char is_upload, const char *name)
{
  memset(&cur_status, 0, sizeof(cur_status));
  cur_status.active = 1;
  cur_status.drive = drive;
  cur_status.is_upload = is_upload;
  strncpy(cur_status.filename, name, sizeof(cur_status.filename) - 1);
  link_ok = 1;
  user_abort = 0;
  cancel = 0;
  unget_have = 0;
  update_progress();
}

/* Ends a transfer: the abort string if this side stopped it, and the
 * result the caller reports. */
static unsigned char finish(unsigned char code)
{
  if (user_abort) code = ZMODEM_ERR_ABORT;
  if (user_abort || cancel) tx_raw(canistr, sizeof(canistr));
  else if (!link_ok) code = ZMODEM_ERR_IO;
  if (code == ZMODEM_OK) cur_status.percent = 100;
  cur_status.active = 0;
  update_progress();
  return code;
}

/* The ZFILE subpacket in the far buffer: name, NUL, decimal size. The
 * name becomes a CBM one: its last path component, characters CBM DOS
 * cannot store dropped, at most 16 long. */
static void parse_zfile(unsigned int n)
{
  unsigned char *p = cur_buf, i, o = 0;
  if (n > 254) n = 254;
  far_read(XFER_FAR, cur_buf, n ? n : 1);
  cur_buf[n] = 0;
  for (i = 0; p[i]; i++) {
    unsigned char c = p[i];
    if (c == '/' || c == '\\') { o = 0; continue; }
    if (c < 32 || c > 126 || c == '*' || c == '?' || c == ',' || c == ':' || c == '=') continue;
    if (o < 16) cur_status.filename[o++] = (char)c;
  }
  if (o == 0) { strcpy(cur_status.filename, "DOWNLOAD"); o = 8; }
  cur_status.filename[o] = 0;
  p += i;
  if (i < n) p++;                             /* past the NUL */
  while (*p == ' ') p++;
  cur_status.file_size = 0;
  while (*p >= '0' && *p <= '9') {
    cur_status.file_size = cur_status.file_size * 10 + (unsigned long)(*p - '0');
    p++;
  }
}

__attribute__((noinline)) unsigned char zmodem_receive(unsigned char drive)
{
  unsigned char type = 0, delim, res;
  unsigned long flags = 0, pos = 0;
  unsigned char errors = 0, file_open = 0, code = ZMODEM_ERR_TIMEOUT;
  unsigned int n;

  begin(drive, 0, "Connecting...");

  /* The ZRQINIT that started this may be waiting in the pushback. */
  rx_header(&type, &flags, 10);
  send_hex_header(ZRINIT, ZRINIT_FLAGS);

  for (;;) {
    if (!rx_header(&type, &flags, HEADER_FRAMES)) {
      if (!link_ok || user_abort) break;
      if (++errors > MAX_ERRORS) break;
      if (file_open) send_hex_header(ZRPOS, pos);
      else send_hex_header(ZRINIT, ZRINIT_FLAGS);
      continue;
    }

    if (type == ZRQINIT) {
      send_hex_header(ZRINIT, ZRINIT_FLAGS);
    } else if (type == ZSINIT) {
      /* The sender's attention string: read and accepted, not used. */
      res = rx_subpacket(&n, &delim);
      if (res == RX_DATA) send_hex_header(ZACK, 0);
      else send_hex_header(ZNAK, 0);
    } else if (type == ZFILE) {
      res = rx_subpacket(&n, &delim);
      if (res == RX_CANCEL) { code = ZMODEM_ERR_ABORT; break; }
      if (res != RX_DATA) { send_hex_header(ZNAK, 0); continue; }
      if (!file_open) {
        unsigned char err;
        parse_zfile(n);
        update_progress();                     /* the box names the file before any question */
        err = cbmdos_create(cur_status.filename, drive);
        if (err == CBMDOS_ERR_EXISTS && ui_confirm_overwrite(cur_status.filename)) {
          cbmdos_delete(cur_status.filename, drive);
          err = cbmdos_create(cur_status.filename, drive);
        }
        if (err != CBMDOS_OK) {                /* refused, or the disk would not take it */
          code = ZMODEM_ERR_IO;
          send_hex_header(ZSKIP, 0);
          continue;
        }
        file_open = 1;
        pos = 0;
        cur_status.bytes_transferred = 0;
        update_progress();
      }
      send_hex_header(ZRPOS, pos);          /* a repeated ZFILE gets the same answer */
    } else if (type == ZDATA) {
      if (!file_open) continue;
      if (flags != pos) {                   /* not where we are: ask again */
        send_hex_header(ZRPOS, pos);
        if (++errors > MAX_ERRORS) break;
        continue;
      }
      for (;;) {
        res = rx_subpacket(&n, &delim);
        if (res == RX_DATA) {
          if (!flush_far(n)) { code = ZMODEM_ERR_IO; cancel = 1; break; }
          pos += n;
          cur_status.bytes_transferred = pos;
          errors = 0;
          update_progress();
          if (delim == ZCRCQ || delim == ZCRCW) send_hex_header(ZACK, pos);
          if (delim == ZCRCG || delim == ZCRCQ) continue;
          break;                            /* ZCRCE, ZCRCW: a header follows */
        }
        if (res == RX_CANCEL) { code = ZMODEM_ERR_ABORT; break; }
        if (!link_ok || user_abort) break;
        if (++errors > MAX_ERRORS) { code = ZMODEM_ERR_CRC; cancel = 1; break; }
        send_hex_header(ZRPOS, pos);        /* bad CRC or a stall: from here again */
        break;
      }
      if (!link_ok || user_abort || cancel || code == ZMODEM_ERR_ABORT) break;
    } else if (type == ZEOF) {
      if (!file_open) continue;
      if (flags != pos) { send_hex_header(ZRPOS, pos); continue; }
      file_open = 0;
      if (cbmdos_close() != CBMDOS_OK) { code = ZMODEM_ERR_IO; cancel = 1; break; }
      code = ZMODEM_OK;
      send_hex_header(ZRINIT, ZRINIT_FLAGS);
    } else if (type == ZFIN) {
      unsigned char c;
      send_hex_header(ZFIN, 0);
      if (rx_raw_byte(&c, 60)) rx_raw_byte(&c, 10);   /* the sender's "OO" */
      if (!file_open && code == ZMODEM_ERR_TIMEOUT) code = ZMODEM_OK;   /* nothing to send */
      break;
    } else {
      send_hex_header(ZNAK, 0);
    }
  }

  if (file_open) cbmdos_close();
  return finish(code);
}

/* ---- sending a file --------------------------------------------------- */

/* Decimal without division: a file on a D81 is under a million bytes. */
static void format_size(char *out, unsigned long v)
{
  static const unsigned long p10[6] = { 100000UL, 10000UL, 1000UL, 100UL, 10UL, 1UL };
  unsigned char i, n = 0;
  for (i = 0; i < 6; i++) {
    unsigned char d = 0;
    while (v >= p10[i]) { v -= p10[i]; d++; }
    if (d || n || i == 5) out[n++] = (char)('0' + d);
  }
  out[n] = 0;
}

static __attribute__((noinline)) void send_zfile_subpacket(const char *filename)
{
  const char *p = filename;
  unsigned int crc = 0;
  char sz_str[8];
  char *sp = sz_str;

  send_hex_header(ZFILE, 0);

  while (*p) {
    crc = z_crc16(crc, (unsigned char)*p);
    tx_escaped((unsigned char)*p++);
  }
  tx_escaped(0);
  crc = z_crc16(crc, 0);

  format_size(sz_str, cur_status.file_size);
  while (*sp) {
    crc = z_crc16(crc, (unsigned char)*sp);
    tx_escaped((unsigned char)*sp++);
  }
  tx_escaped(' ');
  crc = z_crc16(crc, ' ');

  /* Frame delimiter ZCRCW */
  tx_byte(ZDLE);
  tx_byte(ZCRCW);
  crc = z_crc16(crc, ZCRCW);

  tx_escaped((unsigned char)((crc >> 8) & 0xff));
  tx_escaped((unsigned char)(crc & 0xff));
}

/* The file from its start as ZDATA subpackets, one disk block each,
 * polling the stack between them. Returns 0 if the link failed. */
static __attribute__((noinline)) unsigned char send_zdata_blocks(void)
{
  unsigned char err = 0;
  unsigned char type;
  unsigned long flags;
  unsigned int cur_n, next_n;

  send_hex_header(ZDATA, 0);

  cur_n = cbmdos_read_next(cur_buf, &err);
  while (cur_n > 0 && link_ok) {
    unsigned int i;
    unsigned int crc = 0;
    unsigned char delim;

    /* Read ahead one block to know if this is the last chunk */
    next_n = cbmdos_read_next(next_buf, &err);
    delim = (next_n == 0) ? ZCRCW : ZCRCG;

    for (i = 0; i < cur_n; i++) {
      crc = z_crc16(crc, cur_buf[i]);
      tx_escaped(cur_buf[i]);
    }
    cur_status.bytes_transferred += cur_n;

    tx_byte(ZDLE);
    tx_byte(delim);
    crc = z_crc16(crc, delim);
    tx_escaped((unsigned char)((crc >> 8) & 0xff));
    tx_escaped((unsigned char)(crc & 0xff));

    net_poll();
    update_progress();

    if (delim == ZCRCW) {
      /* The receiver acknowledges the final subpacket */
      rx_header(&type, &flags, 300);
    }

    cur_n = next_n;
    if (cur_n > 0) memcpy(cur_buf, next_buf, cur_n);
  }
  cbmdos_close_read();
  return link_ok;
}

__attribute__((noinline)) unsigned char zmodem_send(const char *filename, unsigned char drive, unsigned long file_size)
{
  unsigned char type = 0, attempt;
  unsigned long flags = 0;

  begin(drive, 1, filename);
  cur_status.file_size = (file_size > 0) ? file_size : 254;

  if (cbmdos_open_read(filename, drive) != CBMDOS_OK) return finish(ZMODEM_ERR_IO);

  /* ZRQINIT until the receiver's ZRINIT: rz repeats its ZRINIT every ten
   * seconds, so wait that long each time. */
  for (attempt = 0; attempt < 5; attempt++) {
    send_hex_header(ZRQINIT, 0);
    if (rx_header(&type, &flags, HEADER_FRAMES) && type == ZRINIT) break;
    if (!link_ok || user_abort) break;
  }
  if (type != ZRINIT) {
    cbmdos_close_read();
    return finish(ZMODEM_ERR_TIMEOUT);
  }

  /* ZFILE until ZRPOS; a ZSKIP means the receiver declined the file. */
  for (attempt = 0; attempt < MAX_ERRORS; attempt++) {
    send_zfile_subpacket(filename);
    if (!rx_header(&type, &flags, HEADER_FRAMES)) {
      if (!link_ok || user_abort) break;
      continue;
    }
    if (type == ZRPOS || type == ZSKIP || type == ZFIN) break;
  }
  if (type == ZSKIP || type == ZFIN) {
    cbmdos_close_read();
    send_hex_header(ZFIN, 0);
    return finish(ZMODEM_ERR_IO);
  }
  if (type != ZRPOS) {
    cbmdos_close_read();
    return finish(ZMODEM_ERR_TIMEOUT);
  }

  if (!send_zdata_blocks()) return finish(ZMODEM_ERR_IO);

  /* ZEOF until ZRINIT, then ZFIN, ZFIN, "OO". */
  for (attempt = 0; attempt < 5; attempt++) {
    send_hex_header(ZEOF, cur_status.bytes_transferred);
    if (rx_header(&type, &flags, HEADER_FRAMES) && type == ZRINIT) break;
    if (!link_ok || user_abort) return finish(ZMODEM_ERR_IO);
  }
  if (type != ZRINIT) return finish(ZMODEM_ERR_TIMEOUT);
  send_hex_header(ZFIN, 0);
  if (rx_header(&type, &flags, 150) && type == ZFIN) tx_raw((const unsigned char *)"OO", 2);
  return finish(ZMODEM_OK);
}
