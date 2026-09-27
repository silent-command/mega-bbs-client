#include "ansi.h"
#include "platform/m65_screen.h"

#define STATE_TEXT      0
#define STATE_ESC       1
#define STATE_CSI       2
#define STATE_SCS       3

#define MAX_ARGS 8

static unsigned char state = STATE_TEXT;
static unsigned char cursor_x = 0;
static unsigned char cursor_y = 0;
static unsigned char saved_x = 0;
static unsigned char saved_y = 0;
static ansi_response_fn response_cb = 0;

void ansi_set_response_fn(ansi_response_fn fn)
{
  response_cb = fn;
}

static unsigned char fg_ansi = 7; /* Default light gray / white */
static unsigned char bg_ansi = 0; /* Default black */
static unsigned char is_bold = 0;
static unsigned char is_reverse = 0;

static unsigned int args[MAX_ARGS];
static unsigned char argc = 0;

/* Standard ANSI (0-7) to VGA palette index (0-7) */
static const unsigned char ansi_to_vga[8] = {
  0, /* 0: Black */
  4, /* 1: Red */
  2, /* 2: Green */
  6, /* 3: Yellow/Brown */
  1, /* 4: Blue */
  5, /* 5: Magenta */
  3, /* 6: Cyan */
  7  /* 7: White/Light Gray */
};

static unsigned char get_active_color(void)
{
  unsigned char fg = ansi_to_vga[fg_ansi & 7];
  unsigned char bg = ansi_to_vga[bg_ansi & 7];

  if (is_bold) fg += 8; /* High intensity */
  if (is_reverse) {
    unsigned char tmp = fg;
    fg = bg;
    bg = tmp;
  }
  /* On single-background text mode, black text on non-zero background is invisible.
   * Render glyph in background accent color so button labels and inverted text are visible. */
  if (fg == 0 && bg != 0) {
    return bg;
  }
  return fg;
}

static void handle_sgr(void)
{
  unsigned char i;
  if (argc == 0) {
    args[0] = 0;
    argc = 1;
  }
  for (i = 0; i < argc; i++) {
    unsigned int a = args[i];
    if (a == 0) {
      /* Reset */
      fg_ansi = 7;
      bg_ansi = 0;
      is_bold = 0;
      is_reverse = 0;
    } else if (a == 1) {
      is_bold = 1;
    } else if (a == 5) {
      /* Blink -> treat as bold */
      is_bold = 1;
    } else if (a == 7) {
      is_reverse = 1;
    } else if (a == 22) {
      is_bold = 0;
    } else if (a == 27) {
      is_reverse = 0;
    } else if (a >= 30 && a <= 37) {
      fg_ansi = (unsigned char)(a - 30);
    } else if (a == 39) {
      fg_ansi = 7;
    } else if (a >= 40 && a <= 47) {
      bg_ansi = (unsigned char)(a - 40);
    } else if (a == 49) {
      bg_ansi = 0;
    }
  }
}

static void handle_csi(unsigned char cmd)
{
  unsigned char cols = m65_screen_cols();
  unsigned char rows = m65_screen_rows();
  unsigned int val1 = (argc > 0 && args[0] > 0) ? args[0] : 1;
  unsigned int val2 = (argc > 1 && args[1] > 0) ? args[1] : 1;

  switch (cmd) {
  case 'm': /* SGR - Select Graphic Rendition */
    handle_sgr();
    break;

  case 'H': /* Cursor Position */
  case 'f':
    cursor_y = (val1 > rows) ? (unsigned char)(rows - 1) : (unsigned char)(val1 - 1);
    cursor_x = (val2 > cols) ? (unsigned char)(cols - 1) : (unsigned char)(val2 - 1);
    break;

  case 'A': /* Cursor Up */
    if (cursor_y >= val1) cursor_y -= (unsigned char)val1;
    else cursor_y = 0;
    break;

  case 'B': /* Cursor Down */
    cursor_y += (unsigned char)val1;
    if (cursor_y >= rows) cursor_y = (unsigned char)(rows - 1);
    break;

  case 'C': /* Cursor Forward / Right */
    cursor_x += (unsigned char)val1;
    if (cursor_x >= cols) cursor_x = (unsigned char)(cols - 1);
    break;

  case 'D': /* Cursor Back / Left */
    if (cursor_x >= val1) cursor_x -= (unsigned char)val1;
    else cursor_x = 0;
    break;

  case 'J': /* Erase in Display */
    if (val1 == 2 || (argc == 0 && args[0] == 2)) {
      m65_screen_cls();
      cursor_x = 0;
      cursor_y = 0;
    } else if (val1 == 0) {
      /* Clear from cursor to end of screen */
      unsigned char r;
      for (r = cursor_y + 1; r < rows; r++) {
        m65_screen_clear_row(r, 0x20, get_active_color());
      }
      for (r = cursor_x; r < cols; r++) {
        m65_screen_putc(r, cursor_y, 0x20, get_active_color());
      }
    }
    break;

  case 'K': /* Erase in Line */
    {
      unsigned char c;
      for (c = cursor_x; c < cols; c++) {
        m65_screen_putc(c, cursor_y, 0x20, get_active_color());
      }
    }
    break;

  case 's': /* Save Cursor Position */
    saved_x = cursor_x;
    saved_y = cursor_y;
    break;

  case 'u': /* Restore Cursor Position */
    cursor_x = saved_x;
    cursor_y = saved_y;
    if (cursor_x >= cols) cursor_x = (unsigned char)(cols - 1);
    if (cursor_y >= rows) cursor_y = (unsigned char)(rows - 1);
    break;

  case 'n': /* DSR - Device Status Report */
    if (val1 == 6) {
      /* Cursor Position Report (CPR): response is \x1b[<row>;<col>R (1-indexed) */
      if (response_cb) {
        char rep[16];
        unsigned char r = (unsigned char)(cursor_y + 1);
        unsigned char c = (unsigned char)(cursor_x + 1);
        unsigned char len = 0;
        rep[len++] = '\x1b';
        rep[len++] = '[';
        if (r >= 100) rep[len++] = (char)('0' + (r / 100));
        if (r >= 10)  rep[len++] = (char)('0' + ((r % 100) / 10));
        rep[len++] = (char)('0' + (r % 10));
        rep[len++] = ';';
        if (c >= 100) rep[len++] = (char)('0' + (c / 100));
        if (c >= 10)  rep[len++] = (char)('0' + ((c % 100) / 10));
        rep[len++] = (char)('0' + (c % 10));
        rep[len++] = 'R';
        response_cb((const unsigned char *)rep, len);
      }
    } else if (val1 == 5) {
      /* Status: OK -> \x1b[0n */
      if (response_cb) {
        response_cb((const unsigned char *)"\x1b[0n", 4);
      }
    }
    break;

  case 'c': /* DA - Device Attributes */
    /* Standard VT100 response: \x1b[?1;2c */
    if (response_cb) {
      response_cb((const unsigned char *)"\x1b[?1;2c", 7);
    }
    break;

  default:
    break;
  }
}

void ansi_init(void)
{
  ansi_reset();
}

void ansi_reset(void)
{
  state = STATE_TEXT;
  cursor_x = 0;
  cursor_y = 0;
  saved_x = 0;
  saved_y = 0;
  fg_ansi = 7;
  bg_ansi = 0;
  is_bold = 0;
  is_reverse = 0;
  argc = 0;
}

void ansi_putc(unsigned char c)
{
  unsigned char cols = m65_screen_cols();
  unsigned char rows = m65_screen_rows();

  switch (state) {
  case STATE_TEXT:
    if (c == 0x1b) {
      state = STATE_ESC;
    } else if (c == '\r') {
      cursor_x = 0;
    } else if (c == '\n') {
      cursor_y++;
      if (cursor_y >= rows) {
        m65_screen_scroll_up(0, (unsigned char)(rows - 1));
        cursor_y = (unsigned char)(rows - 1);
      }
    } else if (c == '\b') {
      if (cursor_x > 0) cursor_x--;
    } else if (c == '\t') {
      cursor_x = (unsigned char)((cursor_x + 8) & ~7);
      if (cursor_x >= cols) {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= rows) {
          m65_screen_scroll_up(0, (unsigned char)(rows - 1));
          cursor_y = (unsigned char)(rows - 1);
        }
      }
    } else if (c == 0x07) {
      /* Bell: ignore or brief flash */
    } else if (c < 0x20) {
      /* Ignore unhandled C0 control codes (NUL, SOH keepalive, etc.) */
      break;
    } else {
      /* Printable CP437 character */
      unsigned char draw_ch = c;
      unsigned char draw_col = get_active_color();

      if (c == ' ') {
        if (is_reverse) {
          draw_ch = 0xdb; /* Full block */
          draw_col = (unsigned char)(ansi_to_vga[fg_ansi & 7] + (is_bold ? 8 : 0));
        } else if (bg_ansi != 0) {
          draw_ch = 0xdb; /* Full block in background color */
          draw_col = ansi_to_vga[bg_ansi & 7];
        }
      } else if (c == 0xdc && (fg_ansi & 7) == 0 && (bg_ansi & 7) != 0) {
        /* Lower half block with black FG and colored BG -> upper half block in BG color */
        draw_ch = 0xdf;
        draw_col = ansi_to_vga[bg_ansi & 7];
      } else if (c == 0xdf && (fg_ansi & 7) == 0 && (bg_ansi & 7) != 0) {
        /* Upper half block with black FG and colored BG -> lower half block in BG color */
        draw_ch = 0xdc;
        draw_col = ansi_to_vga[bg_ansi & 7];
      }

      m65_screen_putc(cursor_x, cursor_y, draw_ch, draw_col);
      cursor_x++;
      if (cursor_x >= cols) {
        cursor_x = 0;
        cursor_y++;
        if (cursor_y >= rows) {
          m65_screen_scroll_up(0, (unsigned char)(rows - 1));
          cursor_y = (unsigned char)(rows - 1);
        }
      }
    }
    break;

  case STATE_ESC:
    if (c == '[') {
      state = STATE_CSI;
      argc = 0;
      args[0] = 0;
    } else if (c == '(' || c == ')') {
      state = STATE_SCS;
    } else {
      /* Unknown escape, return to text */
      state = STATE_TEXT;
    }
    break;

  case STATE_SCS:
    state = STATE_TEXT;
    break;

  case STATE_CSI:
    if (c >= '0' && c <= '9') {
      if (argc == 0) argc = 1;
      args[argc - 1] = args[argc - 1] * 10 + (unsigned int)(c - '0');
    } else if (c == ';') {
      if (argc == 0) argc = 1;
      if (argc < MAX_ARGS) {
        argc++;
        args[argc - 1] = 0;
      }
    } else if (c == '?') {
      /* Private mode prefix (DECSET), ignore prefix char */
    } else {
      /* Command terminator */
      handle_csi(c);
      state = STATE_TEXT;
    }
    break;
  }
}

void ansi_write(const unsigned char *buf, unsigned int len)
{
  while (len--) {
    ansi_putc(*buf++);
  }
}

void ansi_get_cursor(unsigned char *x, unsigned char *y)
{
  if (x) *x = cursor_x;
  if (y) *y = cursor_y;
}

void ansi_set_cursor(unsigned char x, unsigned char y)
{
  cursor_x = x;
  cursor_y = y;
}
