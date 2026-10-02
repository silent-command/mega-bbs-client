#include <stdio.h>
#include <assert.h>
#include "platform/m65_screen.h"

extern unsigned char mock_screen_mem[65536];
extern unsigned char mock_colour_mem[65536];
extern unsigned char mock_vic_regs[65536];

int main(void)
{
  printf("Testing screen subsystem...\n");

  /* Simulate starting in 80x25 with Commodore Blue background and border */
  mock_vic_regs[0xd031 & 0xffff] = 0x80;
  mock_vic_regs[0xd07b & 0xffff] = 24;
  mock_vic_regs[0xd020 & 0xffff] = 6;
  mock_vic_regs[0xd021 & 0xffff] = 6;
  mock_vic_regs[0xd030 & 0xffff] = 0x64;   /* BASIC 65: palette RAM for colors 0-15 */
  mock_vic_regs[0xd070 & 0xffff] = 0xff;   /* and text palette bank 3 */
  m65_screen_init();

  /* Startup colors are BASIC's, as the other clients start (2026-10-02) */
  assert(m65_screen_border() == 6);
  assert(m65_screen_bg() == 6);
  assert(m65_screen_orig_border() == 6);
  assert(m65_screen_orig_bg() == 6);

  /* A PETSCII screen keeps BASIC's palette: RAM, bank 3 */
  m65_screen_set_emul(EMUL_PETSCII);
  assert(mock_vic_regs[0xd030 & 0xffff] & 0x04);
  assert(((mock_vic_regs[0xd070 & 0xffff] >> 4) & 3) == 3);
  /* an ANSI screen takes the PC palette in bank 1, and back again */
  m65_screen_set_emul(EMUL_ANSI);
  assert(((mock_vic_regs[0xd070 & 0xffff] >> 4) & 3) == 1);
  m65_screen_set_emul(EMUL_PETSCII);
  assert(((mock_vic_regs[0xd070 & 0xffff] >> 4) & 3) == 3);

  /* Test default resolution: 80x25 */
  assert(m65_screen_cols() == 80);
  assert(m65_screen_rows() == 25);
  assert(m65_screen_res() == RES_80X25);

  /* Test starting from 40x25 mode detection */
  mock_vic_regs[0xd031 & 0xffff] = 0x00;
  mock_vic_regs[0xd07b & 0xffff] = 24;
  m65_screen_init();
  assert(m65_screen_cols() == 40);
  assert(m65_screen_rows() == 25);
  assert(m65_screen_res() == RES_40X25);

  /* Test starting from 80x50 mode detection */
  mock_vic_regs[0xd031 & 0xffff] = 0x88;
  mock_vic_regs[0xd07b & 0xffff] = 49;
  m65_screen_init();
  assert(m65_screen_cols() == 80);
  assert(m65_screen_rows() == 50);
  assert(m65_screen_res() == RES_80X50);

  /* Test switching to 40x25 */
  m65_screen_set_res(RES_40X25);
  assert(m65_screen_cols() == 40);
  assert(m65_screen_rows() == 25);
  assert(m65_screen_res() == RES_40X25);

  /* Test switching to 80x50 */
  m65_screen_set_res(RES_80X50);
  assert(m65_screen_cols() == 80);
  assert(m65_screen_rows() == 50);
  assert(m65_screen_res() == RES_80X50);

  /* Test cycling resolution: 80x50 -> 40x25 -> 80x25 -> 80x50 */
  m65_screen_cycle_res();
  assert(m65_screen_res() == RES_40X25);
  m65_screen_cycle_res();
  assert(m65_screen_res() == RES_80X25);
  m65_screen_cycle_res();
  assert(m65_screen_res() == RES_80X50);

  /* Test drawing primitives */
  m65_screen_set_res(RES_80X25);
  m65_screen_cls();
  m65_screen_putc(10, 5, 'X', 2);
  assert(mock_screen_mem[5 * 80 + 10] == 'X');
  assert(mock_colour_mem[5 * 80 + 10] == 2);

  /* Test scrolling */
  m65_screen_scroll_up(0, 24);
  assert(mock_screen_mem[4 * 80 + 10] == 'X');
  assert(mock_colour_mem[4 * 80 + 10] == 2);
  assert(mock_screen_mem[24 * 80] == 0x20); /* Bottom row cleared */

  /* Test emulation switching */
  assert(m65_screen_emul() == EMUL_PETSCII);
  m65_screen_toggle_emul();
  assert(m65_screen_emul() == EMUL_ANSI);
  m65_screen_toggle_emul();
  assert(m65_screen_emul() == EMUL_PETSCII);

  printf("Screen tests passed successfully!\n");
  return 0;
}
