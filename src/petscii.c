#include "petscii.h"
#include "platform/m65_screen.h"
#include "platform/m65_font.h"

static unsigned char cursor_x = 0;
static unsigned char cursor_y = 0;
static unsigned char cur_color = 1; /* White */
static unsigned char cur_rvs = 0;   /* 0 or 0x80 */

unsigned char petscii_to_screencode(unsigned char p)
{
  if (p >= 0x40 && p <= 0x5f) return (unsigned char)(p - 0x40);
  if (p >= 0x60 && p <= 0x7f) return (unsigned char)(p - 0x20);
  if (p >= 0xa0 && p <= 0xbf) return (unsigned char)(p - 0x40);
  if (p >= 0xc0 && p <= 0xfe) return (unsigned char)(p - 0x80);
  if (p == 0xff) return 0x5e;
  return p;
}

void petscii_init(void)
{
  petscii_reset();
}

void petscii_reset(void)
{
  cursor_x = 0;
  cursor_y = 0;
  cur_color = 1;
  cur_rvs = 0;
}

void petscii_putc(unsigned char c)
{
  unsigned char cols = m65_screen_cols();
  unsigned char rows = m65_screen_rows();

  /* Control codes */
  switch (c) {
  case 0x93: /* Clear screen & Home */
    m65_screen_cls();
    cursor_x = 0;
    cursor_y = 0;
    return;

  case 0x13: /* Home */
    cursor_x = 0;
    cursor_y = 0;
    return;

  case 0x0d: /* Carriage Return */
    cursor_x = 0;
    cursor_y++;
    if (cursor_y >= rows) {
      m65_screen_scroll_up(0, (unsigned char)(rows - 1));
      cursor_y = (unsigned char)(rows - 1);
    }
    return;

  case 0x0a: /* Line Feed (swallowed after CR so 'j' screencode 10 is not printed) */
    return;

  case 0x14: /* DEL / Backspace */
    if (cursor_x > 0) {
      cursor_x--;
      m65_screen_putc(cursor_x, cursor_y, 0x20, cur_color);
    }
    return;

  case 0x11: /* Cursor Down */
    if (cursor_y + 1 < rows) cursor_y++;
    return;

  case 0x91: /* Cursor Up */
    if (cursor_y > 0) cursor_y--;
    return;

  case 0x1d: /* Cursor Right */
    if (cursor_x + 1 < cols) cursor_x++;
    return;

  case 0x9d: /* Cursor Left */
    if (cursor_x > 0) cursor_x--;
    return;

  case 0x12: /* Reverse On */
    cur_rvs = 0x80;
    return;

  case 0x92: /* Reverse Off */
    cur_rvs = 0;
    return;

  case 0x0e: /* Switch to Text / Lowercase */
    m65_font_set_petscii(0);
    return;

  case 0x8e: /* Switch to Uppercase / Graphics */
    m65_font_set_petscii(1);
    return;

  /* Colors */
  case 0x90: cur_color = 0; return;  /* Black */
  case 0x05: cur_color = 1; return;  /* White */
  case 0x1c: cur_color = 2; return;  /* Red */
  case 0x9f: cur_color = 3; return;  /* Cyan */
  case 0x9c: cur_color = 4; return;  /* Purple */
  case 0x1e: cur_color = 5; return;  /* Green */
  case 0x1f: cur_color = 6; return;  /* Blue */
  case 0x9e: cur_color = 7; return;  /* Yellow */
  case 0x81: cur_color = 8; return;  /* Orange */
  case 0x95: cur_color = 9; return;  /* Brown */
  case 0x96: cur_color = 10; return; /* Light Red */
  case 0x97: cur_color = 11; return; /* Dark Grey */
  case 0x98: cur_color = 12; return; /* Grey */
  case 0x99: cur_color = 13; return; /* Light Green */
  case 0x9a: cur_color = 14; return; /* Light Blue */
  case 0x9b: cur_color = 15; return; /* Light Grey */
  case 0x0c: /* Form Feed / Clear screen */
    m65_screen_cls();
    cursor_x = 0;
    cursor_y = 0;
    return;

  case 0x07: /* Bell */
  case 0x08: /* Disable Shift-CBM */
  case 0x09: /* Enable Shift-CBM / Tab */
  case 0x94: /* Insert */
    return;

  default:
    break;
  }

  /* In PETSCII, bytes in 0x00..0x1F and 0x80..0x9F are control codes.
   * Any control codes not handled in the switch above (e.g. 0x01 SOH keepalive,
   * 0x07 BEL, etc.) must be ignored and NEVER printed as screencodes. */
  if (c < 0x20 || (c >= 0x80 && c <= 0x9f)) {
    return;
  }

  /* Printable character */
  {
    unsigned char sc = (unsigned char)(petscii_to_screencode(c) ^ cur_rvs);
    m65_screen_putc(cursor_x, cursor_y, sc, cur_color);
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
}

void petscii_write(const unsigned char *buf, unsigned int len)
{
  while (len--) {
    petscii_putc(*buf++);
  }
}

void petscii_get_cursor(unsigned char *x, unsigned char *y)
{
  if (x) *x = cursor_x;
  if (y) *y = cursor_y;
}

void petscii_set_cursor(unsigned char x, unsigned char y)
{
  cursor_x = x;
  cursor_y = y;
}
