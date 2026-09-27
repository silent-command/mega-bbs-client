#ifndef M65_FONT_H
#define M65_FONT_H

#define ROM_UPPERCASE_GFX 0x2D000UL
#define ROM_LOWERCASE_TXT 0x2D800UL
#define FONT_CP437_RAM    0x11000UL

void m65_font_install(void);
void m65_font_set_ansi(void);
void m65_font_set_petscii(unsigned char graphics);

#endif
