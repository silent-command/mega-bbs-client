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

  /* Test background color space mapping to full block (0xdb) */
  {
    const char *s = "\x1b[1;1H\x1b[44m ";
    ansi_write((const unsigned char *)s, (unsigned int)strlen(s));
    /* Space with Blue BG should be mapped to 0xdb in Blue color (ansi_to_vga[4] = 1) */
    assert(mock_screen_mem[0] == 0xdb);
    assert(mock_colour_mem[0] == 1);
  }

  /* Test SCS escape swallow (\x1b(B) */
  {
    const char *s = "\x1b[1;1H\x1b(BX";
    ansi_write((const unsigned char *)s, (unsigned int)strlen(s));
    /* 'B' must be swallowed by SCS state; only 'X' printed */
    assert(mock_screen_mem[0] == 'X');
  }

  /* Test black foreground with colored background fallback (e.g. BBS buttons) */
  {
    const char *s = "\x1b[1;1H\x1b[0;30;41mInfo";
    ansi_write((const unsigned char *)s, (unsigned int)strlen(s));
    assert(mock_screen_mem[1] == 'n');
    /* ansi_to_vga[1] (Red) is 4 */
    assert(mock_colour_mem[1] == 4);
  }

  printf("ANSI tests passed successfully!\n");
  return 0;
}
