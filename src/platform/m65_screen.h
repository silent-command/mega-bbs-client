#ifndef M65_SCREEN_H
#define M65_SCREEN_H

#define M65_SCREEN_BASE 0x10000UL     /* Screen RAM in Bank 1 */
#define M65_FONT_RAM    0x11000UL     /* CP437 Font in Bank 1 */

#define RES_40X25 0
#define RES_80X25 1
#define RES_80X50 2

#define EMUL_PETSCII 0
#define EMUL_ANSI    1

/* Video initialization and mode setting */
void m65_screen_init(void);
void m65_screen_set_res(unsigned char res);
void m65_screen_cycle_res(void);
unsigned char m65_screen_res(void);
unsigned char m65_screen_cols(void);
unsigned char m65_screen_rows(void);

/* Emulation and palette selection */
void m65_screen_set_emul(unsigned char emul);
void m65_screen_toggle_emul(void);
unsigned char m65_screen_emul(void);
void m65_screen_set_palette_bank(unsigned char bank);

/* Colors */
void m65_screen_set_border(unsigned char col);
void m65_screen_set_bg(unsigned char col);
unsigned char m65_screen_border(void);
unsigned char m65_screen_bg(void);
unsigned char m65_screen_orig_border(void);
unsigned char m65_screen_orig_bg(void);

/* Screen drawing primitives */
void m65_screen_cls(void);
void m65_screen_clear_row(unsigned char row, unsigned char fill_char, unsigned char col);
void m65_screen_scroll_up(unsigned char top_row, unsigned char bot_row);
void m65_screen_putc(unsigned char x, unsigned char y, unsigned char ch, unsigned char col);
void m65_screen_puts(unsigned char x, unsigned char y, const char *s, unsigned char col);
void m65_screen_cursor_enable(unsigned char enable);
void m65_screen_set_cursor(unsigned char x, unsigned char y);

#endif
