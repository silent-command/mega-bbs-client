#include "m65_font.h"

#ifdef __MEGA65__
#include "mega65/memory.h"

void m65_font_install(void)
{
  /* The CP437 font binary is loaded into Bank 1 at FONT_CP437_RAM ($11000)
   * during boot via cbmdos_load("CP437", boot_drive, 0x11000UL, 2048).
   * Nothing to do here if already loaded. */
}

void m65_font_set_ansi(void)
{
  /* Unlock VIC-IV */
  POKE(0xd02f, 0x47);
  POKE(0xd02f, 0x53);
  /* Point CHARPTR ($D068-$D06A) to $11000 (Bank 1) */
  POKE(0xd068, 0x00);
  POKE(0xd069, 0x10);
  POKE(0xd06a, 0x01);
}

void m65_font_set_petscii(unsigned char graphics)
{
  unsigned long addr = graphics ? ROM_UPPERCASE_GFX : ROM_LOWERCASE_TXT;
  /* Unlock VIC-IV */
  POKE(0xd02f, 0x47);
  POKE(0xd02f, 0x53);
  /* Point CHARPTR ($D068-$D06A) to ROM character set in Bank 2 */
  POKE(0xd068, (unsigned char)(addr & 0xff));
  POKE(0xd069, (unsigned char)((addr >> 8) & 0xff));
  POKE(0xd06a, (unsigned char)((addr >> 16) & 0xff));
}

#else

void m65_font_install(void) {}
void m65_font_set_ansi(void) {}
void m65_font_set_petscii(unsigned char graphics) { (void)graphics; }

#endif
