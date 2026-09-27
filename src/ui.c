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

typedef struct {
  const char *name;
  unsigned char primary;   /* Headers, borders, separator lines, F-keys */
  unsigned char secondary; /* Shortcut keys, prompts, brackets */
  unsigned char text;      /* Standard text / labels */
  unsigned char highlight; /* Selected row, highlighted value */
} ui_theme_t;

static const ui_theme_t ui_themes[6] = {
  { "Ice Blue & Silver",  14,  3, 1, 14 }, /* 1: Light Blue & Cyan (Default) */
  { "Phosphor & Mint",    13,  5, 1, 13 }, /* 2: Light Green & Green */
  { "Cyan & Cool White",   3, 14, 1,  3 }, /* 3: Cyan & Light Blue */
  { "Electric Coral",     10,  2, 1, 10 }, /* 4: Light Red & Red */
  { "Lavender & Violet",   4, 14, 1,  4 }, /* 5: Purple & Light Blue */
  { "Monochrome Steel",   15, 12, 1,  1 }  /* 6: Light Grey & Med Grey */
};

static unsigned char active_theme = 0; /* Default: Ice Blue & Silver */
unsigned char work_drive = 0;

/* Text centered on a row. */
static void puts_centered(unsigned char row, const char *text, unsigned char color)
{
  unsigned char cols = m65_screen_cols(), len = (unsigned char)strlen(text);
  m65_screen_puts((cols > len) ? (unsigned char)((cols - len) / 2) : 0, row, text, color);
}


#define THEME_PRI (ui_themes[active_theme].primary)
#define THEME_SEC (ui_themes[active_theme].secondary)
#define THEME_TXT (ui_themes[active_theme].text)
#define THEME_HI  (ui_themes[active_theme].highlight)

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
  if (k) POKE(0xd610, 0);
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
  unsigned char field = (unsigned char)(strlen(prompt) + maxlen + 1);   /* prompt, text, cursor */
  unsigned char start_col = (cols > field) ? (unsigned char)((cols - field) / 2) : 0;
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

    m65_screen_puts(start_col, row, buf, 1);

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

void ui_draw_status(const char *bbs_name, unsigned char emul, unsigned char res, unsigned char drive)
{
  char line[82];
  unsigned char cols = m65_screen_cols();
  unsigned char rows = m65_screen_rows();
  const char *res_str = (res == RES_40X25) ? "40x25" : (res == RES_80X50) ? "80x50" : "80x25";
  const char *emul_str = (emul == EMUL_ANSI) ? "ANSI" : "PET";
  unsigned char n = 0;
  const char *p;

  /* Format: BBS_NAME | EMUL | RES | D:8 | F1:Dir F3:Mode F5:Res F7:Xfer */
  memset(line, ' ', cols);
  line[cols] = 0;

  if (cols >= 80) {
    p = bbs_name ? bbs_name : "Connected";
    while (*p && n < 20) line[n++] = *p++;
    line[n++] = ' '; line[n++] = '|'; line[n++] = ' ';

    p = emul_str;
    while (*p) line[n++] = *p++;
    line[n++] = ' '; line[n++] = '|'; line[n++] = ' ';

    p = res_str;
    while (*p) line[n++] = *p++;
    line[n++] = ' '; line[n++] = '|'; line[n++] = ' ';

    line[n++] = 'D'; line[n++] = ':'; line[n++] = (char)('8' + drive);
    line[n++] = ' '; line[n++] = '|'; line[n++] = ' ';

    p = "F1:Disconnect  F5:Transfer  F7:Drive";
    while (*p && n < cols) line[n++] = *p++;
  } else {
    /* 40 column status line */
    p = emul_str;
    while (*p && n < 4) line[n++] = *p++;
    line[n++] = ' ';
    p = res_str;
    while (*p && n < 10) line[n++] = *p++;
    line[n++] = ' ';
    p = "F1:Disc F5:Xfr F7:Drv";
    while (*p && n < cols) line[n++] = *p++;
  }

  /* Draw on bottom row in secondary theme accent */
  m65_screen_puts(0, (unsigned char)(rows - 1), line, THEME_SEC);
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
    if (IS_KEY_F7(k)) { work_drive ^= 1; continue; }
    if (k == KEY_ESC || k == KEY_STOP || k == KEY_RETURN || IS_KEY_F1(k) || IS_KEY_F5(k)) {
      return TRANSFER_ACT_NONE;
    }
  }
}

static void draw_dir_screen(unsigned char selected, unsigned char top_index)
{
  unsigned char i, count;
  unsigned char cols = m65_screen_cols();
  unsigned char rows = m65_screen_rows();
  unsigned char page_size = (rows > 25) ? 35 : 14;
  unsigned char sep_row = (rows > 25) ? 44 : 20;
  unsigned char title_len = 37; /* "MEGA65 BBS CLIENT - DIALING DIRECTORY" */
  unsigned char title_x = (cols > title_len) ? (unsigned char)((cols - title_len) / 2) : 0;
  char bar[82];

  m65_screen_cls();

  /* Header */
  memset(bar, '=', cols);
  bar[cols] = 0;
  m65_screen_puts(0, 1, bar, THEME_PRI);
  m65_screen_puts(title_x, 2, "MEGA65 BBS CLIENT - DIALING DIRECTORY", 1);
  m65_screen_puts(0, 3, bar, THEME_PRI);

  count = bookmarks_count();
  for (i = 0; i < page_size; i++) {
    unsigned char idx = (unsigned char)(top_index + i);
    bookmark_t *bm;
    char row_str[82];
    unsigned char color;
    unsigned int num;
    const char *em_str;
    const char *rs_str;
    unsigned char pos = 0;
    const char *s;

    if (idx >= count) break;
    bm = bookmarks_get(idx);
    if (!bm) break;

    color = (idx == selected) ? THEME_HI : THEME_TXT;
    num = (unsigned int)(idx + 1);
    em_str = (bm->emul == EMUL_ANSI) ? "ANSI" : "PET ";
    rs_str = (bm->res == RES_40X25) ? "40x25" : (bm->res == RES_80X50) ? "80x50" : (bm->res == RES_40IN80) ? "40/80" : "80x25";

    memset(row_str, ' ', cols);
    row_str[cols] = 0;

    if (cols >= 80) {
      pos = 1; /* 1 space margin on the left */

      /* Selection indicator */
      row_str[pos++] = (idx == selected) ? '>' : ' ';

      /* Numeric entry indexing: right-aligned in 2 digits */
      if (num < 10) {
        row_str[pos++] = ' ';
        row_str[pos++] = (char)('0' + num);
      } else if (num < 100) {
        row_str[pos++] = (char)('0' + (num / 10));
        row_str[pos++] = (char)('0' + (num % 10));
      } else {
        row_str[pos++] = '9';
        row_str[pos++] = '+';
      }
      row_str[pos++] = '.';
      row_str[pos++] = ' ';

      /* Name (up to 20 chars) */
      s = bm->name;
      while (*s && pos < 27) row_str[pos++] = *s++;
      while (pos < 29) row_str[pos++] = ' ';

      /* Host (up to 26 chars) */
      s = bm->host;
      while (*s && pos < 55) row_str[pos++] = *s++;
      while (pos < 57) row_str[pos++] = ' ';

      /* Emulation (4 chars) */
      row_str[pos++] = em_str[0]; row_str[pos++] = em_str[1];
      row_str[pos++] = em_str[2]; row_str[pos++] = em_str[3];
      while (pos < 63) row_str[pos++] = ' ';

      /* Resolution (5 chars), then the speed */
      s = rs_str;
      while (*s && pos < 68) row_str[pos++] = *s++;
      while (pos < 70) row_str[pos++] = ' ';
      s = ui_speed_name(bm->speed);
      while (*s && pos < 75) row_str[pos++] = *s++;
      row_str[cols] = 0;
    } else {
      /* Selection indicator */
      row_str[pos++] = (idx == selected) ? '>' : ' ';

      /* Numeric entry indexing: right-aligned in 2 digits */
      if (num < 10) {
        row_str[pos++] = (char)('0' + num);
        row_str[pos++] = '.';
        row_str[pos++] = ' ';
      } else if (num < 100) {
        row_str[pos++] = (char)('0' + (num / 10));
        row_str[pos++] = (char)('0' + (num % 10));
        row_str[pos++] = '.';
        row_str[pos++] = ' ';
      } else {
        row_str[pos++] = '9';
        row_str[pos++] = '+';
        row_str[pos++] = '.';
      }

      /* 40 columns */
      s = bm->name;
      while (*s && pos < 21) row_str[pos++] = *s++;
      while (pos < 22) row_str[pos++] = ' ';

      row_str[pos++] = em_str[0]; row_str[pos++] = em_str[1];
      row_str[pos++] = em_str[2]; row_str[pos++] = em_str[3];
      row_str[pos++] = ' ';

      s = rs_str;
      while (*s && pos < 32) row_str[pos++] = *s++;
      row_str[cols] = 0;
    }

    /* The 80-column row is 74 characters wide: start it at column 3 so
     * the table sits in the middle, as the header and footer do. */
    if (cols >= 80) {
      row_str[cols - 3] = 0;
      m65_screen_puts(3, (unsigned char)(5 + i), row_str, color);
    } else {
      m65_screen_puts(0, (unsigned char)(5 + i), row_str, color);
    }
  }

  /* Scroll indicator if count > page_size */
  if (count > page_size) {
    char s_info[40], num[8];
    unsigned char shown_to = (unsigned char)(top_index + page_size);
    if (shown_to > count) shown_to = count;
    strcpy(s_info, "[UP/DN: Scroll (");
    strcat(s_info, fmt_uint(num + 7, (unsigned int)(top_index + 1)));
    strcat(s_info, "-");
    strcat(s_info, fmt_uint(num + 7, shown_to));
    strcat(s_info, "/");
    strcat(s_info, fmt_uint(num + 7, count));
    strcat(s_info, ")]");
    puts_centered((unsigned char)(sep_row - 1), s_info, THEME_SEC);
  }

  /* Commands footer */
  memset(bar, '-', cols);
  bar[cols] = 0;
  m65_screen_puts(0, sep_row, bar, THEME_PRI);

  if (cols >= 80) {
    const char *f1_str = "[A] Add  [E] Edit  [D] Delete  [J] Jump  [Q] Quick-Dial  [T] Theme  [X] Exit";
    static char f2_str[] = "F1: Disconnect     F3: Text Mode     F5: Transfer     F7: Drive 8";
    f2_str[sizeof(f2_str) - 2] = (char)('8' + work_drive);
    puts_centered((unsigned char)(sep_row + 1), f1_str, THEME_SEC);
    puts_centered((unsigned char)(sep_row + 2), f2_str, THEME_PRI);
  } else {
    static char f3_str[] = "F1:Disc  F3:Mode  F5:Xfer  F7:Drv 8";
    f3_str[sizeof(f3_str) - 2] = (char)('8' + work_drive);
    puts_centered((unsigned char)(sep_row + 1), "[A]Add  [E]Edit  [D]Del  [J]Jump", THEME_SEC);
    puts_centered((unsigned char)(sep_row + 2), "[Q]Quick-Dial  [T]Theme  [X]Exit", THEME_SEC);
    puts_centered((unsigned char)(sep_row + 3), f3_str, THEME_PRI);
  }
}

void ui_color_demo(void)
{
  unsigned char cols = m65_screen_cols();
  unsigned char rows = m65_screen_rows();
  unsigned char t;
  unsigned char was = active_theme;                 /* restored if the choice is cancelled */
  char bar[82];

  for (;;) {
    m65_screen_cls();
    memset(bar, '=', cols);
    bar[cols] = 0;

    if (cols >= 80) {
      const char *title = "COMPLEMENTARY COLOR SCHEME DEMO & SELECTOR";
      unsigned char tx = (cols > (unsigned char)strlen(title)) ? (unsigned char)((cols - (unsigned char)strlen(title)) / 2) : 0;
      m65_screen_puts(0, 1, bar, THEME_PRI);
      m65_screen_puts(tx, 2, title, 1);
      m65_screen_puts(0, 3, bar, THEME_PRI);

      for (t = 0; t < 6; t++) {
        unsigned char y = (unsigned char)(4 + t * 3);
        char label[48];
        const ui_theme_t *th = &ui_themes[t];

        if (t == active_theme) {
          strcpy(label, "[*] ");
        } else {
          label[0] = '[';
          label[1] = (char)('1' + t);
          label[2] = ']';
          label[3] = ' ';
          label[4] = 0;
        }
        strcat(label, th->name);
        if (t == active_theme) strcat(label, " (ACTIVE)");

        m65_screen_puts(2, y, label, th->primary);
        m65_screen_puts(36, y, "---- Sample Header ----", th->primary);

        m65_screen_puts(4, (unsigned char)(y + 1),
          "[A] Add  [E] Edit  [D] Delete  [Q] Quick-Dial", th->secondary);
        m65_screen_puts(53, (unsigned char)(y + 1),
          "> 1. Sample BBS [Selected]", th->highlight);
      }

      memset(bar, '-', cols);
      bar[cols] = 0;
      m65_screen_puts(0, (unsigned char)(rows - 2), bar, THEME_PRI);
      {
        const char *hint = "Press [1-6] followed by [RETURN], [ESC] to cancel";
        unsigned char hx = (cols > (unsigned char)strlen(hint)) ? (unsigned char)((cols - (unsigned char)strlen(hint)) / 2) : 0;
        m65_screen_puts(hx, (unsigned char)(rows - 1), hint, 1);
      }
    } else {
      /* 40 columns mode */
      m65_screen_puts(0, 0, "==== COLOR SCHEMES ====", THEME_PRI);
      for (t = 0; t < 6; t++) {
        unsigned char y = (unsigned char)(2 + t * 3);
        char label[32];
        const ui_theme_t *th = &ui_themes[t];
        label[0] = (t == active_theme) ? '*' : (char)('1' + t);
        label[1] = '.';
        label[2] = ' ';
        label[3] = 0;
        strcat(label, th->name);
        if (t == active_theme) strcat(label, " *");

        m65_screen_puts(1, y, label, th->primary);
        m65_screen_puts(3, (unsigned char)(y + 1), "[A]Add  [E]Edit  [Q]Dial", th->secondary);
        m65_screen_puts(3, (unsigned char)(y + 2), "> Sample BBS [Selected]", th->highlight);
      }
      m65_screen_puts(0, 23, "[1-6] then RETURN, ESC cancels", 1);
    }

    {
      unsigned char k = ui_wait_key();
      if (k >= '1' && k <= '6') {
        active_theme = (unsigned char)(k - '1');
        continue;
      }
      if (k == KEY_RETURN) break;
      if (k == KEY_ESC || k == KEY_STOP) {
        active_theme = was;
        break;
      }
    }
  }
}

static unsigned char menu_res = RES_80X25;


/* Saves the table and, if the disk refused, says so: a failed save used to
 * return silently after twenty seconds of drive retries. */
static void save_or_warn(unsigned char boot_drive, unsigned char row)
{
  if (!bookmarks_save(boot_drive)) {
    m65_screen_clear_row(row, ' ', 1);
    puts_centered(row, "Could not write BBSCFG. Press any key.", 2);
    ui_wait_key();
  }
}
/* The prompts for a site's fields, prefilled from *bm; 1 when the name,
 * host and port were answered (emulation and resolution keep their values
 * when skipped). Shared by Add and Edit. */
static __attribute__((noinline)) unsigned char edit_site(bookmark_t *bm, unsigned char row)
{
  char p_str[8];
  char opt_str[4];
  unsigned int pt = 0;
  char *p;

  /* One prompt after another on the same row, each centered on its own
   * width, so the row is cleared before every one. */
  p = fmt_uint(p_str + 7, bm->port);
  memmove(p_str, p, strlen(p) + 1);
  m65_screen_clear_row(row, ' ', 1);
  if (!ui_read_line(row, "Name: ", bm->name, NAME_MAX - 1)) return 0;
  m65_screen_clear_row(row, ' ', 1);
  if (!ui_read_line(row, "Host: ", bm->host, HOST_MAX - 1)) return 0;
  m65_screen_clear_row(row, ' ', 1);
  if (!ui_read_line(row, "Port: ", p_str, 6)) return 0;
  for (p = p_str; *p >= '0' && *p <= '9'; p++) pt = pt * 10 + (unsigned int)(*p - '0');
  if (pt) bm->port = pt;

  opt_str[0] = (char)('1' + bm->emul);
  opt_str[1] = 0;
  m65_screen_clear_row(row, ' ', 1);
  if (ui_read_line(row, "Mode [1=PETSCII, 2=ANSI]: ", opt_str, 2) && opt_str[0] >= '1' && opt_str[0] <= '2')
    bm->emul = (unsigned char)(opt_str[0] - '1');

  opt_str[0] = (char)('1' + bm->res);
  opt_str[1] = 0;
  m65_screen_clear_row(row, ' ', 1);
  if (ui_read_line(row, "Res [1=40x25, 2=80x25, 3=80x50, 4=40 in 80]: ", opt_str, 2) && opt_str[0] >= '1' && opt_str[0] <= '4')
    bm->res = (unsigned char)(opt_str[0] - '1');

  opt_str[0] = (char)('1' + bm->speed);
  opt_str[1] = 0;
  m65_screen_clear_row(row, ' ', 1);
  if (ui_read_line(row, "Speed [1=300, 2=1200, 3=2400, 4=9600, 5=max]: ", opt_str, 2) && opt_str[0] >= '1' && opt_str[0] <= '5')
    bm->speed = (unsigned char)(opt_str[0] - '1');
  return 1;
}

/* "300".."9600" or "max", for the directory and the speed message. */
const char *ui_speed_name(unsigned char speed)
{
  static const char *const names[5] = { "300", "1200", "2400", "9600", "max" };
  return names[speed > 4 ? 4 : speed];
}

unsigned char ui_dialing_directory(unsigned char boot_drive)
{
  unsigned char selected = 0;
  unsigned char top_index = 0;
  unsigned char count;

  /* Always restore Main Menu's configured resolution and PETSCII mode */
  m65_screen_set_res(menu_res);
  m65_screen_set_emul(EMUL_PETSCII);

  for (;;) {
    unsigned char k;
    unsigned char page_size;
    unsigned char rows = m65_screen_rows();
    unsigned char sep_row = (rows > 25) ? 44 : 20;
    unsigned char prompt_row = (unsigned char)(sep_row - 2);

    page_size = (rows > 25) ? 35 : 14;
    count = bookmarks_count();

    if (count > 0 && selected >= count) selected = (unsigned char)(count - 1);

    /* Keep selected within visible viewport */
    if (selected < top_index) {
      top_index = selected;
    } else if (selected >= top_index + page_size) {
      top_index = (unsigned char)(selected - page_size + 1);
    }

    draw_dir_screen(selected, top_index);

    k = ui_wait_key();
    if (k == KEY_RETURN) {
      if (count > 0) return selected;
    } else if (k == KEY_UP || k == 'w' || k == 'W') {
      if (selected > 0) {
        selected--;
      } else if (count > 0) {
        selected = (unsigned char)(count - 1);
      }
    } else if (k == KEY_DOWN || k == 's' || k == 'S') {
      if (selected + 1 < count) {
        selected++;
      } else {
        selected = 0;
      }
    } else if (k >= '1' && k <= '9') {
      /* Direct selection 1..9 */
      unsigned char idx = (unsigned char)(k - '1');
      if (idx < count) {
        selected = idx;
        return selected;
      }
    } else if (k == '0') {
      /* Direct selection 10 */
      if (9 < count) {
        selected = 9;
        return selected;
      }
    } else if (k == 'j' || k == 'J') {
      /* Jump shortcut [J] */
      char j_str[8];
      j_str[0] = 0;
      m65_screen_clear_row(prompt_row, ' ', 1);
      m65_screen_clear_row((unsigned char)(prompt_row + 1), ' ', 1);
      if (ui_read_line(prompt_row, "Jump to entry #: ", j_str, 5) && j_str[0]) {
        unsigned int j_val = 0;
        char *jp = j_str;
        while (*jp >= '0' && *jp <= '9') j_val = j_val * 10 + (*jp++ - '0');
        if (j_val >= 1 && j_val <= count) {
          selected = (unsigned char)(j_val - 1);
          return selected;
        }
      }
    } else if (k == 'a' || k == 'A') {
      bookmark_t new_bm;
      memset(&new_bm, 0, sizeof(new_bm));
      new_bm.port = 23;
      new_bm.res = menu_res;
      new_bm.speed = SPEED_MAX;
      if (edit_site(&new_bm, prompt_row)) {
        bookmarks_add(&new_bm);
        save_or_warn(boot_drive, prompt_row);
      }
    } else if (k == 'e' || k == 'E') {
      if (count > 0) {
        bookmark_t *cur = bookmarks_get(selected);
        if (cur) {
          bookmark_t edit_bm = *cur;
          if (edit_site(&edit_bm, prompt_row)) {
            bookmarks_update(selected, &edit_bm);
            save_or_warn(boot_drive, prompt_row);
          }
        }
      }
    } else if (k == 'd' || k == 'D') {
      /* Delete bookmark */
      if (count > 0) {
        bookmarks_delete(selected);
        save_or_warn(boot_drive, prompt_row);
        if (selected > 0) selected--;
      }
    } else if (k == 'q' || k == 'Q' || IS_KEY_F2(k)) {
      /* Quick connect: use hostname as default label */
      static bookmark_t quick_bm;
      char p_str[8];
      memset(&quick_bm, 0, sizeof(quick_bm));
      quick_bm.speed = SPEED_MAX;
      strcpy(p_str, "23");

      m65_screen_clear_row(prompt_row, ' ', 1);
      if (ui_read_line(prompt_row, "Host/IP: ", quick_bm.host, HOST_MAX - 1) &&
          (m65_screen_clear_row(prompt_row, ' ', 1), ui_read_line(prompt_row, "Port (23): ", p_str, 6))) {
        unsigned int pt = 0;
        char *p = p_str;
        while (*p >= '0' && *p <= '9') pt = pt * 10 + (*p++ - '0');
        quick_bm.port = pt ? pt : 23;
        /* Default label to hostname */
        strncpy(quick_bm.name, quick_bm.host, NAME_MAX - 1);
        quick_bm.name[NAME_MAX - 1] = 0;
        quick_bm.emul = EMUL_PETSCII;
        quick_bm.res = menu_res;
        bookmarks_add(&quick_bm);
        return (unsigned char)(bookmarks_count() - 1);
      }
    } else if (IS_KEY_F7(k)) {
      work_drive ^= 1;                            /* the drive transfers use */
    } else if (k == 't' || k == 'T') {
      /* Theme / Color demo */
      ui_color_demo();
      draw_dir_screen(selected, top_index);
      continue;
    } else if (IS_KEY_F3(k)) {
      /* F3 Text Mode toggle */
      m65_screen_cycle_res();
      menu_res = m65_screen_res();
    } else if (k == 'x' || k == 'X' || k == KEY_STOP) {
      /* Quit */
      m65_exit_to_basic();
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
    } else if (k == KEY_STOP || k == KEY_ESC || IS_KEY_F1(k) || IS_KEY_F5(k)) {
      return 0;
    }
  }
}
