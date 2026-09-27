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
  petscii_get_cursor(&x, &y);
  assert(x == 1);
  assert(y == 0);
  assert(mock_screen_mem[0] == 1);
  assert(mock_colour_mem[0] == 1); /* Default white */

  /* Color change: Red ($1C = 2) */
  petscii_putc(0x1c);
  petscii_putc('B');
  assert(mock_screen_mem[1] == 2);
  assert(mock_colour_mem[1] == 2); /* Red */

  /* Reverse video: $12 */
  petscii_putc(0x12);
  petscii_putc('C');
  assert(mock_screen_mem[2] == (3 ^ 0x80)); /* Reverse 'C' */

  /* Reverse off: $92 */
  petscii_putc(0x92);
  petscii_putc('D');
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

  printf("PETSCII tests passed successfully!\n");
  return 0;
}
