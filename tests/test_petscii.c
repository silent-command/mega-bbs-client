#include <stdio.h>
#include <assert.h>
#include "petscii.h"
#include "platform/m65_screen.h"

extern unsigned char mock_screen_mem[65536];
extern unsigned char mock_colour_mem[65536];

int main(void)
{
  unsigned char x, y;

  printf("Testing PETSCII parser...\n");

  m65_screen_init();
  m65_screen_set_res(RES_80X25);
  petscii_init();

  /* Character mapping */
  assert(petscii_to_screencode('A') == 1);
  assert(petscii_to_screencode('@') == 0);
  assert(petscii_to_screencode(' ') == 32);

  /* Output 'A' */
  petscii_putc('A');
  petscii_flush();
  petscii_get_cursor(&x, &y);
  assert(x == 1);
  assert(y == 0);
  assert(mock_screen_mem[0] == 1);
  assert(mock_colour_mem[0] == 1); /* Default white */

  /* Color change: Red ($1C = 2) */
  petscii_putc(0x1c);
  petscii_putc('B');
  petscii_flush();
  assert(mock_screen_mem[1] == 2);
  assert(mock_colour_mem[1] == 2); /* Red */

  /* Reverse video: $12 */
  petscii_putc(0x12);
  petscii_putc('C');
  petscii_flush();
  assert(mock_screen_mem[2] == (3 ^ 0x80)); /* Reverse 'C' */

  /* Reverse off: $92 */
  petscii_putc(0x92);
  petscii_putc('D');
  petscii_flush();
  assert(mock_screen_mem[3] == 4);

  /* Clear screen: $93 */
  petscii_putc(0x93);
  petscii_get_cursor(&x, &y);
  assert(x == 0);
  assert(y == 0);

  /* CR: $0D */
  petscii_putc('X');
  petscii_putc(0x0d);
  petscii_get_cursor(&x, &y);
  assert(x == 0);
  assert(y == 1);

  /* Control codes 0x01 (SOH keepalive) and 0x07 (BEL) must NOT print or advance cursor */
  petscii_putc(0x93); /* Clear screen */
  petscii_putc(0x01); /* SOH keepalive */
  petscii_putc(0x07); /* BEL */
  petscii_putc(0x00); /* NUL */
  petscii_putc(0x08); /* Lock */
  petscii_get_cursor(&x, &y);
  assert(x == 0);
  assert(y == 0);
  assert(mock_screen_mem[0] == 32); /* Still empty space */

  /* Flash on and off is the blink attribute */
  petscii_putc(0x05);
  petscii_putc(0x0f);
  petscii_putc('F');
  petscii_putc(0x8f);
  petscii_putc('G');
  petscii_flush();
  assert(mock_colour_mem[0] == (1 | 0x10) && mock_colour_mem[1] == 1);

  /* Insert opens a blank at the cursor and the rest of the line moves right */
  petscii_putc(0x93);
  petscii_write((const unsigned char *)"ABC", 3);
  petscii_putc(0x9d);
  petscii_putc(0x9d);
  petscii_putc(0x94);
  petscii_flush();
  assert(mock_screen_mem[0] == 1 && mock_screen_mem[1] == 32 && mock_screen_mem[2] == 2 && mock_screen_mem[3] == 3);

  /* A 40-column window on the 80-column screen: text starts at column 20
   * and wraps at 40; the board's rows are still the screen's */
  petscii_set_window(40, 20);
  petscii_putc(0x93);
  {
    unsigned char i;
    for (i = 0; i < 41; i++) petscii_putc('X');
    petscii_flush();
  }
  assert(mock_screen_mem[20] == 24 && mock_screen_mem[59] == 24 && mock_screen_mem[60] == 32);
  assert(mock_screen_mem[80 + 20] == 24);
  petscii_get_cursor(&x, &y);
  assert(x == 1 && y == 1);
  petscii_set_window(0, 0);

  printf("PETSCII tests passed successfully!\n");
  return 0;
}
