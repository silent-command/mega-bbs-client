/* ANSI-BBS rendering onto the text screen.
 *
 * The VIC-IV text mode has one background colour for the whole screen,
 * so a cell cannot show a foreground and a background of its own. The
 * VIC-III attribute bits fill the gap: a coloured background is drawn as
 * the cell in reverse video in that colour (the glyph goes dark), which
 * is exact for reverse-video text and for black-on-colour, and keeps
 * coloured bars whole where earlier code left gaps. Blink and underline
 * are hardware; bold is the bright half of the palette; iCE colours
 * (CSI ?33h) make blink mean a bright background instead. */
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
static unsigned char scroll_top = 0, scroll_bot = 24;
static ansi_response_fn response_cb = 0;

void ansi_set_response_fn(ansi_response_fn fn)
{
  response_cb = fn;
}

static unsigned char fg_ansi = 7;       /* 0-15: ANSI colour, 8-15 the bright ones */
static unsigned char bg_ansi = 0;
static unsigned char is_bold = 0, is_blink = 0, is_reverse = 0, is_under = 0;
static unsigned char ice_colors = 0;

static unsigned char args[MAX_ARGS];    /* an argument above 255 means nothing on this screen */
static unsigned char argc = 0;
static unsigned char priv = 0;

/* Standard ANSI (0-7) to VGA palette index (0-7) */
static const unsigned char ansi_to_vga[8] = { 0, 4, 2, 6, 1, 5, 3, 7 };

/* The colour byte for the next cell: colour in the low nibble, the
 * attributes above it. */
static __attribute__((noinline)) unsigned char cell_color(void)
{
  unsigned char fg = (unsigned char)(ansi_to_vga[fg_ansi & 7] | (((fg_ansi & 8) || is_bold) ? 8 : 0));
  unsigned char bg = (unsigned char)(ansi_to_vga[bg_ansi & 7] | (((bg_ansi & 8) || (is_blink && ice_colors)) ? 8 : 0));
  unsigned char attr = 0;
  if (is_under) attr |= 0x80;
  if (is_blink && !ice_colors) attr |= 0x10;
  if (is_reverse) return (unsigned char)(fg | attr | 0x20);
  if (bg != 0) return (unsigned char)(bg | attr | 0x20);
  return (unsigned char)(fg | attr);
}

static __attribute__((noinline)) void handle_sgr(void)
{
  unsigned char i;
  if (argc == 0) {
    args[0] = 0;
    argc = 1;
  }
  for (i = 0; i < argc; i++) {
    unsigned char a = args[i];
    if (a == 0) {
      fg_ansi = 7; bg_ansi = 0;
      is_bold = is_blink = is_reverse = is_under = 0;
    } else if (a == 1) is_bold = 1;
    else if (a == 2 || a == 22) is_bold = 0;
    else if (a == 4) is_under = 1;
    else if (a == 24) is_under = 0;
    else if (a == 5 || a == 6) is_blink = 1;
    else if (a == 25) is_blink = 0;
    else if (a == 7) is_reverse = 1;
    else if (a == 27) is_reverse = 0;
    else if (a >= 30 && a <= 37) fg_ansi = (unsigned char)(a - 30);
    else if (a == 39) fg_ansi = 7;
    else if (a >= 40 && a <= 47) bg_ansi = (unsigned char)(a - 40);
    else if (a == 49) bg_ansi = 0;
    else if (a >= 90 && a <= 97) fg_ansi = (unsigned char)(a - 90 + 8);
    else if (a >= 100 && a <= 107) bg_ansi = (unsigned char)(a - 100 + 8);
  }
}

static __attribute__((noinline)) void erase(unsigned char x0, unsigned char y0, unsigned char x1, unsigned char y1)
{
  unsigned char col = cell_color(), x, y;
  for (y = y0; y <= y1; y++) {
    unsigned char a = (y == y0) ? x0 : 0, b = (y == y1) ? x1 : (unsigned char)(m65_screen_cols() - 1);
    for (x = a; x <= b; x++) m65_screen_putc_buf(x, y, 0x20, col);
  }
}

/* A line feed within the scroll region. */
static __attribute__((noinline)) void line_feed(void)
{
  if (cursor_y == scroll_bot) m65_screen_scroll_up(scroll_top, scroll_bot);
  else if (cursor_y < m65_screen_rows() - 1) cursor_y++;
}

static __attribute__((noinline)) void handle_csi(unsigned char cmd)
{
  unsigned char cols = m65_screen_cols();
  unsigned char rows = m65_screen_rows();
  unsigned char val1 = (argc > 0 && args[0] > 0) ? args[0] : 1;
  unsigned char val2 = (argc > 1 && args[1] > 0) ? args[1] : 1;
  unsigned char raw1 = (argc > 0) ? args[0] : 0;

  if (priv) {
    if (cmd == 'h' && raw1 == 33) ice_colors = 1;      /* iCE colours: blink is a bright background */
    if (cmd == 'l' && raw1 == 33) ice_colors = 0;
    return;                                             /* other private modes (cursor visibility, wrap) are left alone */
  }

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

  case 'J': /* Erase in Display: 0 to the end, 1 from the start, 2 all */
    if (raw1 == 2) {
      m65_screen_cls();
      cursor_x = 0;
      cursor_y = 0;
    } else if (raw1 == 1) {
      erase(0, 0, cursor_x, cursor_y);
    } else {
      erase(cursor_x, cursor_y, (unsigned char)(cols - 1), (unsigned char)(rows - 1));
    }
    break;

  case 'K': /* Erase in Line: 0 to the end, 1 from the start, 2 whole */
    if (raw1 == 2) erase(0, cursor_y, (unsigned char)(cols - 1), cursor_y);
    else if (raw1 == 1) erase(0, cursor_y, cursor_x, cursor_y);
    else erase(cursor_x, cursor_y, (unsigned char)(cols - 1), cursor_y);
    break;

  case 'r': /* Scroll region */
    {
      unsigned char top = (unsigned char)(val1 - 1);
      unsigned char bot = (argc > 1 && args[1] > 0 && args[1] <= rows) ? (unsigned char)(args[1] - 1) : (unsigned char)(rows - 1);
      if (top < bot && bot < rows) { scroll_top = top; scroll_bot = bot; }
      else { scroll_top = 0; scroll_bot = (unsigned char)(rows - 1); }
      cursor_x = 0;
      cursor_y = scroll_top;
    }
    break;

  case 'L': /* Insert lines at the cursor, the region's last lines fall off */
    if (cursor_y >= scroll_top && cursor_y <= scroll_bot)
      while (val1--) m65_screen_scroll_down(cursor_y, scroll_bot);
    break;

  case 'M': /* Delete lines at the cursor */
    if (cursor_y >= scroll_top && cursor_y <= scroll_bot)
      while (val1--) m65_screen_scroll_up(cursor_y, scroll_bot);
    break;

  case 'S': /* Scroll the region up */
    while (val1--) m65_screen_scroll_up(scroll_top, scroll_bot);
    break;

  case 'T': /* Scroll the region down */
    while (val1--) m65_screen_scroll_down(scroll_top, scroll_bot);
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
      if (response_cb) response_cb((const unsigned char *)"\x1b[0n", 4);
    }
    break;

  case 'c': /* DA - Device Attributes: a VT100 with advanced video */
    if (response_cb) response_cb((const unsigned char *)"\x1b[?1;2c", 7);
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
  scroll_top = 0;
  scroll_bot = (unsigned char)(m65_screen_rows() - 1);
  fg_ansi = 7;
  bg_ansi = 0;
  is_bold = is_blink = is_reverse = is_under = 0;
  ice_colors = 0;
  argc = 0;
  priv = 0;
}

void ansi_putc(unsigned char c)
{
  unsigned char cols = m65_screen_cols();

  switch (state) {
  case STATE_TEXT:
    if (c == 0x1b) {
      state = STATE_ESC;
    } else if (c == '\r') {
      cursor_x = 0;
    } else if (c == '\n') {
      line_feed();
    } else if (c == '\b') {
      if (cursor_x > 0) cursor_x--;
    } else if (c == '\t') {
      cursor_x = (unsigned char)((cursor_x + 8) & ~7);
      if (cursor_x >= cols) {
        cursor_x = 0;
        line_feed();
      }
    } else if (c < 0x20) {
      /* Unhandled C0 controls (NUL, SOH keepalive, BEL) are ignored */
      break;
    } else {
      m65_screen_putc_buf(cursor_x, cursor_y, c, cell_color());
      cursor_x++;
      if (cursor_x >= cols) {
        cursor_x = 0;
        line_feed();
      }
    }
    break;

  case STATE_ESC:
    if (c == '[') {
      state = STATE_CSI;
      argc = 0;
      args[0] = 0;
      priv = 0;
    } else if (c == '(' || c == ')') {
      state = STATE_SCS;
    } else {
      state = STATE_TEXT;
    }
    break;

  case STATE_SCS:
    state = STATE_TEXT;
    break;

  case STATE_CSI:
    if (c >= '0' && c <= '9') {
      if (argc == 0) argc = 1;
      args[argc - 1] = (args[argc - 1] < 25) ? (unsigned char)(args[argc - 1] * 10 + (c - '0')) : 255;
    } else if (c == ';') {
      if (argc == 0) argc = 1;
      if (argc < MAX_ARGS) {
        argc++;
        args[argc - 1] = 0;
      }
    } else if (c == '?') {
      priv = 1;
    } else {
      handle_csi(c);
      state = STATE_TEXT;
    }
    break;
  }
}

/* Writes what is buffered and puts the cursor where the text ends. */
void ansi_flush(void)
{
  m65_screen_flush();
  m65_screen_set_cursor(cursor_x, cursor_y);
}

void ansi_write(const unsigned char *buf, unsigned int len)
{
  while (len--) {
    ansi_putc(*buf++);
  }
  ansi_flush();
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
