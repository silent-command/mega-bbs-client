#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "ansi.h"
#include "platform/m65_screen.h"

extern unsigned char mock_screen_mem[65536];
extern unsigned char mock_colour_mem[65536];

static unsigned char last_resp[32];
static unsigned int last_resp_len = 0;

static void test_resp(const unsigned char *data, unsigned int len)
{
  if (len < sizeof(last_resp)) {
    memcpy(last_resp, data, len);
    last_resp_len = len;
  }
}

int main(void)
{
  unsigned char x, y;

  printf("Testing ANSI parser...\n");

  m65_screen_init();
  m65_screen_set_res(RES_80X25);
  ansi_init();

  /* Plain text output */
  ansi_putc('H');
  ansi_putc('e');
  ansi_putc('l');
  ansi_putc('l');
  ansi_putc('o');
  ansi_flush();

  ansi_get_cursor(&x, &y);
  assert(x == 5);
  assert(y == 0);
  assert(mock_screen_mem[0] == 'H');
  assert(mock_screen_mem[1] == 'e');
  assert(mock_screen_mem[2] == 'l');
  assert(mock_screen_mem[3] == 'l');
  assert(mock_screen_mem[4] == 'o');

  /* Cursor movement: ESC [ 10 ; 20 H */
  ansi_write((const unsigned char *)"\x1b[10;20H", 8);
  ansi_get_cursor(&x, &y);
  assert(y == 9);
  assert(x == 19);

  /* Cursor up/down/left/right */
  ansi_write((const unsigned char *)"\x1b[2A", 4); /* Up 2 */
  ansi_get_cursor(&x, &y);
  assert(y == 7);

  ansi_write((const unsigned char *)"\x1b[3B", 4); /* Down 3 */
  ansi_get_cursor(&x, &y);
  assert(y == 10);

  ansi_write((const unsigned char *)"\x1b[5C", 4); /* Right 5 */
  ansi_get_cursor(&x, &y);
  assert(x == 24);

  ansi_write((const unsigned char *)"\x1b[4D", 4); /* Left 4 */
  ansi_get_cursor(&x, &y);
  assert(x == 20);

  /* SGR colors: ESC [ 1 ; 31 m (Bold Red) */
  ansi_write((const unsigned char *)"\x1b[1;31m", 7);
  ansi_putc('R');
  ansi_flush();
  /* Red (4) + Bold (8) = 12 */
  assert(mock_screen_mem[10 * 80 + 20] == 'R');
  assert(mock_colour_mem[10 * 80 + 20] == 12);

  /* Clear screen: ESC [ 2 J */
  ansi_write((const unsigned char *)"\x1b[2J", 4);
  ansi_get_cursor(&x, &y);
  assert(x == 0);
  assert(y == 0);

  /* Test DSR CPR (\x1b[6n) and DA (\x1b[0c) responses */
  {
    ansi_set_response_fn(test_resp);

    /* Move to row 10 (index 9), col 20 (index 19) */
    ansi_write((const unsigned char *)"\x1b[10;20H", 8);
    last_resp_len = 0;
    ansi_write((const unsigned char *)"\x1b[6n", 4);
    assert(last_resp_len == 8);
    assert(memcmp(last_resp, "\x1b[10;20R", 8) == 0);

    /* Device Attributes \x1b[0c */
    last_resp_len = 0;
    ansi_write((const unsigned char *)"\x1b[0c", 4);
    assert(last_resp_len == 7);
    assert(memcmp(last_resp, "\x1b[?1;2c", 7) == 0);
  }

  /* A coloured background is the cell in reverse video in that colour */
  {
    const char *s = "\x1b[1;1H\x1b[0;44m X";
    ansi_write((const unsigned char *)s, (unsigned int)strlen(s));
    assert(mock_screen_mem[0] == ' ' && mock_colour_mem[0] == (1 | 0x20));   /* blue (ansi_to_vga[4] = 1), reverse */
    assert(mock_screen_mem[1] == 'X' && mock_colour_mem[1] == (1 | 0x20));   /* the glyph stays, the bar is whole */
  }

  /* Reverse video, blink, underline, bright colours, iCE colours */
  {
    const char *s = "\x1b[1;1H\x1b[0;7;31mR\x1b[0;5;32mB\x1b[0;4;33mU\x1b[0;91mH\x1b[0;101m ";
    ansi_write((const unsigned char *)s, (unsigned int)strlen(s));
    assert(mock_colour_mem[0] == (4 | 0x20));          /* reverse: red cell, dark glyph */
    assert(mock_colour_mem[1] == (2 | 0x10));          /* blink attribute */
    assert(mock_colour_mem[2] == (6 | 0x80));          /* underline */
    assert(mock_colour_mem[3] == 12);                  /* bright red without bold */
    assert(mock_colour_mem[4] == (12 | 0x20));         /* bright red background */
    s = "\x1b[?33h\x1b[1;1H\x1b[0;5;44m ";                /* iCE: blink is a bright background */
    ansi_write((const unsigned char *)s, (unsigned int)strlen(s));
    assert(mock_colour_mem[0] == (9 | 0x20));
    ansi_write((const unsigned char *)"\x1b[?33l", 6);
  }

  /* Erase forms: K 1 and 2, J 1 */
  {
    const char *s = "\x1b[2J\x1b[0mabcdef\x1b[1;3H\x1b[1K";
    ansi_write((const unsigned char *)s, (unsigned int)strlen(s));
    assert(mock_screen_mem[0] == ' ' && mock_screen_mem[2] == ' ' && mock_screen_mem[3] == 'd');
    ansi_write((const unsigned char *)"\x1b[2K", 4);
    assert(mock_screen_mem[3] == ' ' && mock_screen_mem[5] == ' ');
    s = "\x1b[2Jrow1\r\nrow2\r\nrow3\x1b[2;3H\x1b[1J";
    ansi_write((const unsigned char *)s, (unsigned int)strlen(s));
    assert(mock_screen_mem[0] == ' ' && mock_screen_mem[80] == ' ' && mock_screen_mem[82] == ' ');
    assert(mock_screen_mem[83] == '2' && mock_screen_mem[160] == 'r');
    ansi_write((const unsigned char *)"\x1b[J", 3);              /* no argument means to the end */
    assert(mock_screen_mem[83] == ' ' && mock_screen_mem[160] == ' ');
  }

  /* Scroll region, insert and delete line */
  {
    const char *s = "\x1b[2J\x1b[0mL1\r\nL2\r\nL3\r\nL4\r\nL5\x1b[2;4r\x1b[4;1H\n";
    ansi_write((const unsigned char *)s, (unsigned int)strlen(s));
    /* rows 2-4 scrolled up: L3 L4 blank; L1 and L5 untouched */
    assert(mock_screen_mem[1] == '1');
    assert(mock_screen_mem[80 + 1] == '3' && mock_screen_mem[160 + 1] == '4' && mock_screen_mem[240 + 1] == ' ');
    assert(mock_screen_mem[320 + 1] == '5');
    ansi_get_cursor(&x, &y);
    assert(y == 3);
    ansi_write((const unsigned char *)"\x1b[2;1H\x1b[L", 9);      /* insert a line at row 2 */
    assert(mock_screen_mem[80 + 1] == ' ' && mock_screen_mem[160 + 1] == '3' && mock_screen_mem[240 + 1] == '4');
    assert(mock_screen_mem[320 + 1] == '5');                      /* outside the region */
    ansi_write((const unsigned char *)"\x1b[M", 3);              /* and delete it again */
    assert(mock_screen_mem[80 + 1] == '3' && mock_screen_mem[240 + 1] == ' ');
    ansi_write((const unsigned char *)"\x1b[r", 3);              /* the whole screen again */
    ansi_get_cursor(&x, &y);
    assert(x == 0 && y == 0);
  }

  /* Test SCS escape swallow (\x1b(B) */
  {
    const char *s = "\x1b[1;1H\x1b(BX";
    ansi_write((const unsigned char *)s, (unsigned int)strlen(s));
    /* 'B' must be swallowed by SCS state; only 'X' printed */
    assert(mock_screen_mem[0] == 'X');
  }

  /* Black on a coloured background: the cell in that colour, reversed */
  {
    const char *s = "\x1b[1;1H\x1b[0;30;41mInfo";
    ansi_write((const unsigned char *)s, (unsigned int)strlen(s));
    assert(mock_screen_mem[1] == 'n');
    assert(mock_colour_mem[1] == (4 | 0x20));
  }

  printf("ANSI tests passed successfully!\n");
  return 0;
}
