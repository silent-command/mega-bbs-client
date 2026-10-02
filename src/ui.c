#include <string.h>
#include "ui.h"
#include "platform/m65_screen.h"
#include "platform/m65_font.h"
#include "platform/m65_exit.h"
#include "netutil.h"
#include "platform/m65_far.h"

#ifdef __MEGA65__
#include "mega65/memory.h"
#include "platform/m65_cbmdos.h"
#else
static unsigned char mock_last_key = 0;
#define PEEK(addr) 0
#define POKE(addr, val) ((void)val)
#endif

/* The menus' text color, which MEGA-F cycles; the in-session boxes use it
 * too. The six themes went with the family layout (2026-10-02). */
unsigned char ui_fg = 1;
unsigned char ui_last_mods;
unsigned char work_drive = 0;



#define THEME_PRI ui_fg
#define THEME_SEC ui_fg
#define THEME_TXT ui_fg
#define THEME_HI  ui_fg

/* The decimal digits of v, ending just before *end (which becomes the
 * NUL); returns where they start. */
static __attribute__((noinline)) char *fmt_uint(char *end, unsigned int v)
{
  *end = 0;
  do { *--end = (char)('0' + v % 10); v /= 10; } while (v);
  return end;
}

#define BOX_CENTER 0xff

/* One row of a bordered box of width w at x: text between the bars at
 * offset pad (BOX_CENTER to center it), or a border row when text is 0. */
static __attribute__((noinline)) void box_row(unsigned char x, unsigned char y, unsigned char w,
                                              const char *text, unsigned char pad, unsigned char color)
{
  char line[82];
  unsigned char inner = (unsigned char)(w - 2), i, n;
  if (!text) {
    line[0] = '+';
    for (i = 1; i < w - 1; i++) line[i] = '-';
    line[w - 1] = '+';
    line[w] = 0;
    m65_screen_puts(x, y, line, THEME_PRI);
    return;
  }
  for (i = 0; i < inner; i++) line[i] = ' ';
  n = (unsigned char)strlen(text);
  if (pad == BOX_CENTER) pad = (inner > n) ? (unsigned char)((inner - n) / 2) : 0;
  if (n > inner - pad) n = (unsigned char)(inner - pad);
  memcpy(line + pad, text, n);
  line[inner] = 0;
  m65_screen_putc(x, y, '|', THEME_PRI);
  m65_screen_puts((unsigned char)(x + 1), y, line, color);
  m65_screen_putc((unsigned char)(x + w - 1), y, '|', THEME_PRI);
}

unsigned char ui_key(void)
{
#ifdef __MEGA65__
  unsigned char k = PEEK(0xd610);
  if (k) { ui_last_mods = PEEK(0xd611); POKE(0xd610, 0); }   /* the modifiers, then the pop */
  return k;
#else
  return 0;
#endif
}

unsigned char ui_wait_key(void)
{
  for (;;) {
    unsigned char k = ui_key();
    if (k) return k;
    net_poll();
  }
}

unsigned char ui_read_line(unsigned char row, const char *prompt, char *out, unsigned char maxlen)
{
  unsigned char len = (unsigned char)strlen(out);
  unsigned char cols = m65_screen_cols();
  unsigned char start_col = 0;                    /* the left edge, as the other clients prompt */
  unsigned char first_key = (len > 0) ? 1 : 0;
  char buf[82];

  for (;;) {
    unsigned char k;
    unsigned char n = 0;
    const char *p = prompt;

    while (*p && n < cols - 2 - start_col) buf[n++] = *p++;
    p = out;
    while (*p && n < cols - 2 - start_col) buf[n++] = *p++;
    buf[n++] = '_';
    while (n < cols - start_col) buf[n++] = ' ';
    buf[cols - start_col] = 0;

    m65_screen_puts(start_col, row, buf, ui_fg);

    k = ui_wait_key();
    if (k == KEY_RETURN) return 1;
    if (k == KEY_STOP || k == KEY_ESC) return 0;

    if (first_key) {
      first_key = 0;
      if (k == KEY_DEL) {
        len = 0;
        out[0] = 0;
        continue;
      }
      if (k >= 0x20 && k < 0x7f) {
        len = 0;
        out[0] = 0;
      }
    }

    if (k == KEY_DEL) {
      if (len > 0) out[--len] = 0;
    } else if (k >= 0x20 && k < 0x7f) {
      if (len < maxlen) {
        out[len++] = (char)k;
        out[len] = 0;
      }
    }
  }
}

/* The transfer box: title, file, and a percentage bar, or a block count
 * when the size is unknown (XMODEM carries none). Shared by both
 * protocols: it was two copies. */
static __attribute__((noinline)) void draw_progress(const char *title, const char *filename,
                                                    unsigned char percent, unsigned int blocks, unsigned char full)
{
  unsigned char cols = m65_screen_cols();
  unsigned char rows = m65_screen_rows();
  unsigned char start_y = (rows > 25) ? 18 : 8;
  unsigned char width = (cols >= 80) ? 50 : 38;
  unsigned char inner_w = (unsigned char)(width - 2);
  unsigned char start_x = (unsigned char)((cols - width) / 2);
  unsigned char bar_len = (unsigned char)(inner_w - 12);
  char line[64];
  unsigned char i;

  /* Borders, title and file rows only when the box first appears or the
   * file changes; later calls redraw the bar. */
  if (full) {
    box_row(start_x, start_y, width, 0, 0, 0);
    box_row(start_x, (unsigned char)(start_y + 4), width, 0, 0, 0);
    box_row(start_x, (unsigned char)(start_y + 1), width, title, BOX_CENTER, THEME_SEC);
    memcpy(line, "File: ", 6);
    strncpy(line + 6, filename, 20);
    line[26] = 0;
    box_row(start_x, (unsigned char)(start_y + 2), width, line, BOX_CENTER, THEME_TXT);
  }
  if (blocks == 0xffff) {
    unsigned char filled = (unsigned char)((percent * bar_len) / 100);
    line[0] = '[';
    for (i = 0; i < bar_len; i++) line[1 + i] = (i < filled) ? '=' : ' ';
    line[1 + bar_len] = ']';
    line[2 + bar_len] = ' ';
    line[3 + bar_len] = (char)('0' + (percent / 100));
    line[4 + bar_len] = (char)('0' + ((percent % 100) / 10));
    line[5 + bar_len] = (char)('0' + (percent % 10));
    line[6 + bar_len] = '%';
    line[7 + bar_len] = 0;
  } else {
    memcpy(line, "Blocks: ", 8);
    strcpy(line + 8, fmt_uint(line + 20, blocks));
  }
  box_row(start_x, (unsigned char)(start_y + 3), width, line, 1, THEME_TXT);
}

/* "NAME exists. Overwrite? [Y/N]" under the transfer box; 1 for yes. */
unsigned char ui_confirm_overwrite(const char *name)
{
  unsigned char row = (m65_screen_rows() > 25) ? 23 : 13;
  unsigned char len = (unsigned char)(strlen(name) + 25), cols = m65_screen_cols();
  unsigned char col = (cols > len) ? (unsigned char)((cols - len) / 2) : 0;
  unsigned char k, yes = 0;
  m65_screen_clear_row(row, ' ', 1);
  m65_screen_puts(col, row, name, 2);
  m65_screen_puts((unsigned char)(col + strlen(name)), row, " exists. Overwrite? [Y/N]", 2);
  for (;;) {
    k = ui_wait_key();
    if (k == 'y' || k == 'Y') { yes = 1; break; }
    if (k == 'n' || k == 'N' || k == KEY_ESC || k == KEY_STOP || k == KEY_RETURN) break;
  }
  m65_screen_clear_row(row, ' ', 1);
  return yes;
}

void ui_draw_zmodem_progress(const zmodem_status_t *st)
{
  draw_progress(st->is_upload ? "[ ZMODEM UPLOAD ]" : "[ ZMODEM DOWNLOAD ]", st->filename, st->percent, 0xffff,
                st->bytes_transferred == 0);
}

void ui_draw_xmodem_progress(const xmodem_status_t *st)
{
  draw_progress(st->is_upload ? "[ XMODEM UPLOAD ]" : "[ XMODEM DOWNLOAD ]", st->filename, st->percent,
                st->file_size ? 0xffff : st->blocks, st->bytes_transferred == 0);
}

unsigned char ui_transfer_menu(void)
{
  unsigned char cols = m65_screen_cols();
  unsigned char rows = m65_screen_rows();
  unsigned char width = (cols >= 80) ? 38 : 34;
  unsigned char start_x = (unsigned char)((cols - width) / 2);
  unsigned char start_y = (rows > 25) ? 18 : 7;
  static char title[] = "[ FILE TRANSFER - DRIVE 8 ]";   /* the digit follows work_drive */
  static const char *const opts[] = {
    "[1] ZMODEM Upload", "[2] ZMODEM Download", "[3] XMODEM Upload", "[4] XMODEM Download", "[ESC] Cancel"
  };
  unsigned char r;

  box_row(start_x, start_y, width, 0, 0, 0);
  for (r = 0; r < 5; r++)
    box_row(start_x, (unsigned char)(start_y + 2 + r), width, opts[r], 1, (r == 4) ? THEME_TXT : THEME_SEC);
  box_row(start_x, (unsigned char)(start_y + 7), width, "", 0, THEME_TXT);
  box_row(start_x, (unsigned char)(start_y + 8), width, 0, 0, 0);

  for (;;) {
    unsigned char k;
    title[24] = (char)('8' + work_drive);
    box_row(start_x, (unsigned char)(start_y + 1), width, title, BOX_CENTER, THEME_PRI);
    k = ui_wait_key();
    if (k == '1') return TRANSFER_ACT_Z_UP;
    if (k == '2') return TRANSFER_ACT_Z_DOWN;
    if (k == '3') return TRANSFER_ACT_X_UP;
    if (k == '4') return TRANSFER_ACT_X_DOWN;
    if (IS_KEY_F5(k)) { work_drive ^= 1; continue; }
    if (k == KEY_ESC || k == KEY_STOP || k == KEY_RETURN || IS_KEY_F1(k) || IS_KEY_F3(k)) {
      return TRANSFER_ACT_NONE;
    }
  }
}

/* ---- the dialing directory, in the family's layout ------------------------
 * The FTP, SFTP, SSH and gopher clients' shape (2026-10-02): the title on
 * the top row, the sites below with the selection in reverse video, an
 * info row, the status row for notices and prompts, and the keys on the
 * last row. MEGA-F and MEGA-B cycle the text and background colors here,
 * and only here: in a session every key belongs to the board. The six
 * color themes, their screen and the boxed table are gone. */

static unsigned char menu_res = 0xff;     /* BASIC's mode, taken at the first visit; each site has its own */
static unsigned char menu_bg, menu_border;
static bookmark_t quick_bm;
bookmark_t *ui_quick_site(void) { return &quick_bm; }

static char row_buf[82];                  /* a row being drawn */
static char ent[82];                      /* an entry being built */
static unsigned char en;

#define ROW_FIRST 2
#define ROW_INFO ((unsigned char)(m65_screen_rows() - 3))
#define ROW_STATUS ((unsigned char)(m65_screen_rows() - 2))
#define ROW_KEYS ((unsigned char)(m65_screen_rows() - 1))
#define PAGE ((unsigned char)(m65_screen_rows() - 5))
#define WIDE (m65_screen_cols() >= 80)

void ui_row(unsigned char row, const char *a, const char *b, unsigned char rev)
{
  unsigned char cols = m65_screen_cols(), n = 0;
  if (a) while (*a && n < cols) row_buf[n++] = *a++;
  if (b) while (*b && n < cols) row_buf[n++] = *b++;
  while (n < cols) row_buf[n++] = ' ';
  row_buf[n] = 0;
  if (rev) m65_screen_puts_rev(0, row, row_buf, ui_fg);
  else m65_screen_puts(0, row, row_buf, ui_fg);
}

void ui_status(const char *a, const char *b) { ui_row(ROW_STATUS, a, b, 0); }

static __attribute__((noinline)) void cat_to(const char *s, unsigned char stop) { while (*s && en < stop) ent[en++] = *s++; }
static __attribute__((noinline)) void pad_to(unsigned char col) { while (en < col) ent[en++] = ' '; }

static const char *res_name(unsigned char res)
{
  static const char *const names[4] = { "40x25", "80x25", "80x50", "40/80" };
  return names[res & 3];
}

/* One site's row: its number (the one J takes), name, host and port,
 * emulation, screen mode, speed. */
static __attribute__((noinline)) void draw_entry(unsigned char idx, unsigned char row, unsigned char rev)
{
  bookmark_t *bm = bookmarks_get(idx);
  char num[8];
  const char *nn;
  en = 0;
  if (!bm) { ui_row(row, 0, 0, 0); return; }
  nn = fmt_uint(num + 7, (unsigned int)(idx + 1));
  pad_to((unsigned char)(4 - strlen(nn)));                /* right-aligned in three columns, then a dot */
  cat_to(nn, 4); cat_to(".", 5); pad_to(6);
  if (WIDE) {
    cat_to(bm->name, 30); pad_to(31);
    {                                     /* the host gets what the port leaves of columns 31-60 */
      const char *pn = fmt_uint(num + 7, bm->port);
      cat_to(bm->host, (unsigned char)(60 - strlen(pn))); cat_to(":", 61); cat_to(pn, 61); pad_to(62);
    }
    cat_to(bm->emul == EMUL_ANSI ? "ANSI" : "PET", 66); pad_to(67);
    cat_to(res_name(bm->res), 72); pad_to(73);   /* the speed ends by column 76, clear of the edge */
  } else {
    cat_to(bm->name, 21); pad_to(22);
    cat_to(bm->emul == EMUL_ANSI ? "ANSI" : "PET", 26); pad_to(27);
    cat_to(res_name(bm->res), 32); pad_to(33);
  }
  cat_to(ui_speed_name(bm->speed), (unsigned char)(en + 4));
  ent[en] = 0;
  ui_row(row, ent, 0, rev);
}

static __attribute__((noinline)) void draw_info(unsigned char top)
{
  unsigned char count = bookmarks_count();
  char num[8];
  en = 0;
  if (!count) cat_to("no sites yet: A adds one, Q dials one without saving it", 79);
  else {
    cat_to(fmt_uint(num + 7, count), 79);
    cat_to(count == 1 ? " site" : " sites", 79);
    if (count > PAGE) {
      cat_to(WIDE ? "   page " : " p", 79);
      cat_to(fmt_uint(num + 7, (unsigned int)(top / PAGE + 1)), 79);
      cat_to(WIDE ? " of " : "/", 79);
      cat_to(fmt_uint(num + 7, (unsigned int)((count + PAGE - 1) / PAGE)), 79);
    }
    cat_to(WIDE ? "   transfers on unit " : "  unit ", 79);
    ent[en++] = (char)('8' + work_drive);
  }
  ent[en] = 0;
  ui_row(ROW_INFO, ent, 0, 0);
}

static void draw_keys(void)
{
  ui_row(ROW_KEYS, WIDE ? "RETURN dial  A add  E edit  D delete  Q quick  J jump  MEGA-F/B color"
                        : "RET dial A add E edit D del Q quick", 0, 0);
}

static void notice(void)
{
  ui_status(WIDE ? "RUN/STOP quits. In a session: F1 disconnect  F3 transfer  F5 drive  F7 speed"
                 : "RUN/STOP quits. J jump, MEGA-F/B color", 0);
}

static __attribute__((noinline)) void draw_page(unsigned char selected, unsigned char top)
{
  unsigned char r, idx, count = bookmarks_count();
  ui_row(0, "MEGA65 BBS Client - version " BBS_VERSION, 0, 0);
  ui_row(1, 0, 0, 0);
  for (r = 0; r < PAGE; r++) {
    idx = (unsigned char)(top + r);
    if (idx < count) draw_entry(idx, (unsigned char)(ROW_FIRST + r), (unsigned char)(idx == selected));
    else ui_row((unsigned char)(ROW_FIRST + r), 0, 0, 0);
  }
  draw_info(top);
  draw_keys();
}

/* The questions for a site, prefilled from *bm, on the status row with the
 * choices for each on the info row; 1 when all were answered. RETURN keeps
 * a value, RUN/STOP anywhere abandons the lot. Shared by Add, Edit and
 * Quick-Dial. */
static __attribute__((noinline)) unsigned char ask_choice(const char *what, const char *choices,
                                                          unsigned char *val, unsigned char max)
{
  char opt[4];
  opt[0] = (char)('1' + *val); opt[1] = 0;
  ui_row(ROW_INFO, choices, 0, 0);
  if (!ui_read_line(ROW_STATUS, what, opt, 1)) return 0;
  if (opt[0] >= '1' && opt[0] < (char)('1' + max)) *val = (unsigned char)(opt[0] - '1');
  return 1;
}

static __attribute__((noinline)) unsigned char edit_site(bookmark_t *bm, const char *what)
{
  char p_str[8];
  unsigned int pt = 0;
  char *p = fmt_uint(p_str + 7, bm->port);
  memmove(p_str, p, strlen(p) + 1);
  ui_row(ROW_INFO, what, ": RETURN keeps what is shown, RUN/STOP cancels", 0);
  if (!ui_read_line(ROW_STATUS, "Name: ", bm->name, NAME_MAX - 1)) return 0;
  if (!ui_read_line(ROW_STATUS, "Host: ", bm->host, HOST_MAX - 1)) return 0;
  if (!ui_read_line(ROW_STATUS, "Port: ", p_str, 5)) return 0;
  for (p = p_str; *p >= '0' && *p <= '9'; p++) pt = pt * 10 + (unsigned int)(*p - '0');
  if (pt) bm->port = pt;
  return ask_choice("Emulation: ", "1 PETSCII  2 ANSI", &bm->emul, 2) &&
         ask_choice("Screen: ", "1 40x25  2 80x25  3 80x50  4 40 in 80", &bm->res, 4) &&
         ask_choice("Speed: ", "1 300  2 1200  3 2400  4 9600  5 max", &bm->speed, 5);
}

/* "300".."9600" or "max", for the directory and the speed message. */
const char *ui_speed_name(unsigned char speed)
{
  static const char *const names[5] = { "300", "1200", "2400", "9600", "max" };
  return names[speed > 4 ? 4 : speed];
}

/* Saves the list and says so if the disk refused. */
static void save_or_warn(unsigned char boot_drive)
{
  if (!bookmarks_save(boot_drive)) ui_status("could not write BBSCFG to the disk", 0);
}

/* The colors, MEGA-F and MEGA-B, as the other clients have them: the text
 * color skips the background's, and the background and border move
 * together (ftp's m65_screen.c), starting from black. */
static void cycle_fg(void)
{
  ui_fg = (unsigned char)((ui_fg + 1) & 15);
  if (ui_fg == menu_bg) ui_fg = (unsigned char)((ui_fg + 1) & 15);
}

static void cycle_bg(void)
{
  menu_bg = (unsigned char)((menu_bg + 1) & 15);  /* from black, where this client starts */
  if (menu_bg == ui_fg) menu_bg = (unsigned char)((menu_bg + 1) & 15);
  menu_border = menu_bg;
  m65_screen_set_bg(menu_bg);
  m65_screen_set_border(menu_border);
}

unsigned char ui_dialing_directory(unsigned char boot_drive)
{
  static unsigned char selected, top;     /* kept, so a session returns to its site */
  unsigned char k, mods, count, old;

  if (menu_res == 0xff) {
    menu_res = m65_screen_res();
    menu_bg = menu_border = 0;            /* black on black with white text, the client's start (main.c) */
  }
  m65_screen_set_res(menu_res);
  m65_screen_set_emul(EMUL_PETSCII);
  m65_screen_set_bg(menu_bg);
  m65_screen_set_border(menu_border);
  m65_screen_cls();

  count = bookmarks_count();
  if (selected >= count) selected = count ? (unsigned char)(count - 1) : 0;
  top = (unsigned char)(selected - selected % PAGE);
  draw_page(selected, top);
  notice();

  for (;;) {
    count = bookmarks_count();
    k = ui_wait_key();
    mods = ui_last_mods;
    if (k >= 0xc1 && k <= 0xda && (mods & 0x08)) {          /* MEGA+letter: the capital with bit 7 set (ssh 5.29) */
      k = (unsigned char)(k & 0x7f);
      if (k == 'F') { cycle_fg(); draw_page(selected, top); notice(); }
      else if (k == 'B') cycle_bg();
      continue;
    }
    old = selected;
    if (k == KEY_DOWN) { if (selected + 1 < count) selected++; }
    else if (k == KEY_UP) { if (selected) selected--; }
    else if (k == KEY_RIGHT) { if (top + PAGE < count) selected = (unsigned char)(top + PAGE); }
    else if (k == KEY_LEFT) selected = (unsigned char)(top >= PAGE ? top - PAGE : 0);
    else if (k == KEY_HOME) selected = 0;
    else if (k == KEY_CLR) { if (count) selected = (unsigned char)(count - 1); }
    else if (k == KEY_RETURN) { if (count) return selected; continue; }
    else if (k == 'j' || k == 'J') {
      char j_str[6];
      unsigned int j = 0;
      char *jp = j_str;
      j_str[0] = 0;
      if (count && ui_read_line(ROW_STATUS, "Jump to site number: ", j_str, 3)) {
        while (*jp >= '0' && *jp <= '9') j = j * 10 + (unsigned int)(*jp++ - '0');
        if (j >= 1 && j <= count) selected = (unsigned char)(j - 1);
      }
      notice();
    } else if (k == 'a' || k == 'A') {
      bookmark_t new_bm;
      if (count >= BOOKMARK_MAX) { ui_status("the directory is full: D deletes a site", 0); continue; }
      memset(&new_bm, 0, sizeof(new_bm));
      new_bm.port = 23;
      new_bm.res = menu_res;
      new_bm.speed = SPEED_MAX;
      if (edit_site(&new_bm, "A new site")) {
        bookmarks_add(&new_bm);
        selected = (unsigned char)(count);
        notice();
        save_or_warn(boot_drive);
      } else ui_status("cancelled", 0);
      draw_info(top); draw_keys();
    } else if (k == 'e' || k == 'E') {
      bookmark_t *cur = count ? bookmarks_get(selected) : 0;
      if (cur) {
        bookmark_t edit_bm = *cur;
        if (edit_site(&edit_bm, "Editing")) {
          bookmarks_update(selected, &edit_bm);
          notice();
          save_or_warn(boot_drive);
          draw_entry(selected, (unsigned char)(ROW_FIRST + selected - top), 1);
        } else ui_status("cancelled", 0);
        draw_info(top); draw_keys();
      }
    } else if (k == 'd' || k == 'D') {
      bookmark_t *cur = count ? bookmarks_get(selected) : 0;
      if (cur) {
        ui_status("Delete ", cur->name);
        ui_row(ROW_INFO, "Y deletes it, any other key keeps it", 0, 0);
        k = ui_wait_key();
        if (k == 'y' || k == 'Y') {
          bookmarks_delete(selected);
          if (selected && selected >= bookmarks_count()) selected--;
          top = (unsigned char)(selected - selected % PAGE);
          draw_page(selected, top);
          notice();
          save_or_warn(boot_drive);
        } else { notice(); draw_info(top); }
      }
      continue;
    } else if (k == 'q' || k == 'Q') {
      /* dialed without joining the list (2026-10-02) */
      memset(&quick_bm, 0, sizeof(quick_bm));
      quick_bm.port = 23;
      quick_bm.res = menu_res;
      quick_bm.speed = SPEED_MAX;
      if (edit_site(&quick_bm, "Quick-Dial, not saved") && quick_bm.host[0]) {
        if (!quick_bm.name[0]) { strncpy(quick_bm.name, quick_bm.host, NAME_MAX - 1); quick_bm.name[NAME_MAX - 1] = 0; }
        return UI_QUICK_DIAL;
      }
      ui_status("cancelled", 0);
      draw_info(top); draw_keys();
    } else if (IS_KEY_F5(k)) {
      work_drive ^= 1;
      draw_info(top);
    } else if (k == KEY_STOP || k == 'x' || k == 'X') {
      m65_exit_to_basic();
    }
    /* the selection moved, or the list changed under it */
    if (selected < top || selected >= top + PAGE || k == 'a' || k == 'A') {
      top = (unsigned char)(selected - selected % PAGE);
      draw_page(selected, top);
    } else if (selected != old) {
      draw_entry(old, (unsigned char)(ROW_FIRST + old - top), 0);
      draw_entry(selected, (unsigned char)(ROW_FIRST + selected - top), 1);
      draw_info(top);
    }
  }
}

#define PICKER_MAX_FILES 32

typedef struct {
  char name[18];
  unsigned int blocks;
} picker_entry_t;

/* The listing lives in bank 1 after the site table; pk is the one near
 * entry (m65_far.h). */
#define PICKER_FAR (FAR_BASE + 0x450UL)
#define PK_OFF(i) ((unsigned int)(i) * (unsigned int)sizeof(picker_entry_t))
static picker_entry_t pk;
static unsigned char picker_count = 0;

static __attribute__((noinline)) void pk_read(unsigned char i)
{
  far_read(PICKER_FAR + PK_OFF(i), &pk, sizeof(pk));
}

static __attribute__((noinline)) void pk_write(unsigned char i)
{
  far_write(&pk, PICKER_FAR + PK_OFF(i), sizeof(pk));
}

static void format_picker_box(unsigned char start_x, unsigned char start_y, unsigned char width, unsigned char height, const char *title)
{
  unsigned char r;
  box_row(start_x, start_y, width, 0, 0, 0);
  box_row(start_x, (unsigned char)(start_y + 1), width, title, BOX_CENTER, THEME_SEC);
  box_row(start_x, (unsigned char)(start_y + 2), width, 0, 0, 0);
  for (r = 3; r < height - 1; r++) box_row(start_x, (unsigned char)(start_y + r), width, "", 0, THEME_TXT);
  box_row(start_x, (unsigned char)(start_y + height - 1), width, 0, 0, 0);
}

/* A typed name has no directory entry to hand: measure the file, so the
 * receiver is told the real size and the bar means something. */
static unsigned long typed_file_size(const char *name, unsigned char drive)
{
#ifdef __MEGA65__
  unsigned long n = cbmdos_file_size(name, drive);
  return n ? n : 254;
#else
  (void)name; (void)drive;
  return 254;
#endif
}

unsigned char ui_file_picker(unsigned char drive, char *out_filename, unsigned long *out_size)
{
  unsigned char cols = m65_screen_cols();
  unsigned char rows = m65_screen_rows();
  unsigned char width = (cols >= 80) ? 44 : 38;
  unsigned char height = (rows > 25) ? 16 : 13;
  unsigned char inner_w = (unsigned char)(width - 2);
  unsigned char start_x = (unsigned char)((cols - width) / 2);
  unsigned char start_y = (rows > 25) ? 14 : 3;
  unsigned char list_rows = (unsigned char)(height - 5); /* Number of visible file slots */
  unsigned char selected = 0;
  unsigned char top_index = 0;
  char title_buf[48];
  char line[64];

  picker_count = 0;

#ifdef __MEGA65__
  /* Read directory from CBM-DOS */
  if (cbmdos_dir_first(drive) == CBMDOS_OK) {
    char name[20];
    unsigned char type;
    unsigned int blocks;
    while (cbmdos_dir_next(name, &type, &blocks) && picker_count < PICKER_MAX_FILES) {
      if (name[0] && (type & 0x0f) != 0) {
        strncpy(pk.name, name, sizeof(pk.name) - 1);
        pk.name[sizeof(pk.name) - 1] = 0;
        pk.blocks = blocks;
        pk_write(picker_count);
        picker_count++;
      }
    }
    cbmdos_dir_end();
  }
#else
  /* Mock files for host test */
  strcpy(pk.name, "BBSCFG");
  pk.blocks = 3;
  pk_write(0);
  strcpy(pk.name, "TEST.TXT");
  pk.blocks = 2;
  pk_write(1);
  picker_count = 2;
#endif

  strcpy(title_buf, "Upload File (Drive  )");
  title_buf[19] = (char)('8' + drive);
  format_picker_box(start_x, start_y, width, height, title_buf);

  box_row(start_x, (unsigned char)(start_y + height - 2), width, "[CR] Send  [M] Manual  [F1] Cancel", BOX_CENTER, THEME_SEC);

  if (picker_count == 0) {
    box_row(start_x, (unsigned char)(start_y + 4), width, "No files found on drive", 3, 2);

    /* Allow manual typing fallback */
    out_filename[0] = 0;
    if (ui_read_line((unsigned char)(start_y + 6), "File: ", out_filename, 16) && out_filename[0]) {
      *out_size = typed_file_size(out_filename, drive);
      return 1;
    }
    return 0;
  }

  for (;;) {
    unsigned char r;
    unsigned char k;

    /* Adjust scroll window */
    if (selected < top_index) top_index = selected;
    if (selected >= top_index + list_rows) top_index = (unsigned char)(selected - list_rows + 1);

    /* Render visible files */
    for (r = 0; r < list_rows; r++) {
      unsigned char idx = (unsigned char)(top_index + r);
      unsigned char y = (unsigned char)(start_y + 3 + r);
      unsigned char is_sel = (idx == selected);
      unsigned char col = is_sel ? THEME_HI : THEME_TXT;
      unsigned char i;

      for (i = 0; i < inner_w; i++) line[i] = ' ';
      line[inner_w] = 0;

      if (idx < picker_count) {
        char blk_str[8];
        unsigned char nlen;
        char *p;
        unsigned char plen;

        pk_read(idx);
        nlen = (unsigned char)strlen(pk.name);
        line[1] = is_sel ? '>' : ' ';
        if (nlen > inner_w - 12) nlen = (unsigned char)(inner_w - 12);
        memcpy(line + 3, pk.name, nlen);
        p = fmt_uint(blk_str + 7, pk.blocks);
        plen = (unsigned char)strlen(p);
        memcpy(line + inner_w - 6 - plen, p, plen);
        memcpy(line + inner_w - 5, " blk ", 5);
      }
      box_row(start_x, y, width, line, 0, col);
    }

    k = ui_wait_key();
    if (k == KEY_UP) {
      if (selected > 0) selected--;
    } else if (k == KEY_DOWN) {
      if (selected + 1 < picker_count) selected++;
    } else if (k == KEY_RETURN) {
      pk_read(selected);
      strcpy(out_filename, pk.name);
      *out_size = (unsigned long)pk.blocks * 254;
      return 1;
    } else if (k == 'm' || k == 'M') {
      out_filename[0] = 0;
      if (ui_read_line((unsigned char)(start_y + height - 2), "File: ", out_filename, 16) && out_filename[0]) {
        *out_size = typed_file_size(out_filename, drive);
        return 1;
      }
      return 0;
    } else if (k == KEY_STOP || k == KEY_ESC || IS_KEY_F1(k) || IS_KEY_F3(k)) {
      return 0;
    }
  }
}
