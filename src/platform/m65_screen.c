#include "m65_font.h"
#include "m65_screen.h"

#ifdef __MEGA65__
#include "mega65/memory.h"

#define SCREEN M65_SCREEN_BASE
#define COLOUR_RAM 0xff80000UL

#else
#include <stdint.h>
#include <string.h>

unsigned char mock_screen_mem[65536];
unsigned char mock_colour_mem[65536];
unsigned char mock_vic_regs[65536];

#define PEEK(addr) (mock_vic_regs[(addr) & 0xffff])
#define POKE(addr, val) (mock_vic_regs[(addr) & 0xffff] = (unsigned char)(val))
#define SCREEN 0x10000UL
#define COLOUR_RAM 0x20000UL

static void lfill(unsigned long dst, unsigned char val, unsigned int len)
{
  if (len == 0) return;
  if (dst >= COLOUR_RAM && dst < COLOUR_RAM + 65536)
    memset(mock_colour_mem + (dst - COLOUR_RAM), val, len);
  else if (dst >= SCREEN && dst < SCREEN + 65536)
    memset(mock_screen_mem + (dst - SCREEN), val, len);
}

static void lcopy(unsigned long src, unsigned long dst, unsigned int len)
{
  if (len == 0) return;
  if (src >= COLOUR_RAM && src < COLOUR_RAM + 65536)
    memmove(mock_colour_mem + (dst - COLOUR_RAM), mock_colour_mem + (src - COLOUR_RAM), len);
  else if (src >= SCREEN && src < SCREEN + 65536)
    memmove(mock_screen_mem + (dst - SCREEN), mock_screen_mem + (src - SCREEN), len);
}

static void lpoke(unsigned long addr, unsigned char val)
{
  if (addr >= COLOUR_RAM && addr < COLOUR_RAM + 65536)
    mock_colour_mem[addr - COLOUR_RAM] = val;
  else if (addr >= SCREEN && addr < SCREEN + 65536)
    mock_screen_mem[addr - SCREEN] = val;
}

#endif

static unsigned char cur_res = RES_80X25;
static unsigned char cur_emul = EMUL_PETSCII;
static unsigned char cur_cols = 80;
static unsigned char cur_rows = 25;
static unsigned char cur_border = 0;  /* Standard startup: Black */
static unsigned char cur_bg = 0;      /* Standard startup: Black */
static unsigned char cur_text = 1;    /* Standard startup: White */
static unsigned char orig_border = 6; /* Pre-launch Commodore Blue */
static unsigned char orig_bg = 6;     /* Pre-launch Commodore Blue */

static const unsigned char vga_r[16] = {
  0x00, 0x00, 0x00, 0x00, 0xaa, 0xaa, 0xaa, 0xaa,
  0x55, 0x55, 0x55, 0x55, 0xff, 0xff, 0xff, 0xff
};
static const unsigned char vga_g[16] = {
  0x00, 0x00, 0xaa, 0xaa, 0x00, 0x00, 0x55, 0xaa,
  0x55, 0x55, 0xff, 0xff, 0x55, 0x55, 0xff, 0xff
};
static const unsigned char vga_b[16] = {
  0x00, 0xaa, 0x00, 0xaa, 0x00, 0xaa, 0x00, 0xaa,
  0x55, 0xff, 0x55, 0xff, 0x55, 0xff, 0x55, 0xff
};

static void init_vga_palette(void)
{
  unsigned char i;
  unsigned char orig_d070;

  /* Unlock VIC-IV */
  POKE(0xd02f, 0x47);
  POKE(0xd02f, 0x53);

  /* Map palette bank 1 at $D100-$D3FF (bits 7-6 = 01 -> 0x40) */
  orig_d070 = PEEK(0xd070);
  POKE(0xd070, (unsigned char)((orig_d070 & 0x3f) | 0x40));

  for (i = 0; i < 16; i++) {
    POKE(0xd100 + i, vga_r[i]);
    POKE(0xd200 + i, vga_g[i]);
    POKE(0xd300 + i, vga_b[i]);
  }

  /* Restore edit bank mapping */
  POKE(0xd070, orig_d070);
}

void m65_screen_set_palette_bank(unsigned char bank)
{
  /* bank: 0 = Commodore, 1 = PC VGA (bits 5-4 of $D070) */
  unsigned char v = PEEK(0xd070);
  v = (unsigned char)((v & 0xcf) | ((bank & 0x03) << 4));
  POKE(0xd070, v);
}

void m65_screen_set_res(unsigned char res)
{
  cur_res = res;
  if (cur_res == RES_40X25) {
    cur_cols = 40;
    cur_rows = 25;
  } else if (cur_res == RES_80X50) {
    cur_cols = 80;
    cur_rows = 50;
  } else {
    cur_res = RES_80X25;
    cur_cols = 80;
    cur_rows = 25;
  }

  /* Unlock VIC-IV registers */
  POKE(0xd02f, 0x47);
  POKE(0xd02f, 0x53);

  /* Configure VIC-IV for resolution */
  if (cur_cols == 80) {
    POKE(0xd031, (unsigned char)(PEEK(0xd031) | 0x80)); /* H640 on */
    POKE(0xd04c, 0x50);                                 /* H640 horizontal position: 80 ($50) */
    POKE(0xd04d, (unsigned char)(PEEK(0xd04d) & 0xf0)); /* Clear TEXTXPOS MSB */
    POKE(0xd058, 80);                                   /* LINESTEP = 80 */
    POKE(0xd059, 0);
    POKE(0xd05e, 80);                                   /* DISP_COLS = 80 */
  } else {
    POKE(0xd031, (unsigned char)(PEEK(0xd031) & 0x7f)); /* H640 off (40 cols) */
    POKE(0xd04c, 0x4e);                                 /* 40-column horizontal position: 78 ($4E) */
    POKE(0xd04d, (unsigned char)(PEEK(0xd04d) & 0xf0)); /* Clear TEXTXPOS MSB */
    POKE(0xd058, 40);                                   /* LINESTEP = 40 */
    POKE(0xd059, 0);
    POKE(0xd05e, 40);                                   /* DISP_COLS = 40 */
  }

  if (cur_rows == 50) {
    POKE(0xd031, (unsigned char)(PEEK(0xd031) | 0x08)); /* V400 on */
    POKE(0xd05b, 0);                                    /* CHRYSCL = 0 (8 rasters per char) */
    POKE(0xd07b, 49);                                   /* 50 text rows */
  } else {
    POKE(0xd031, (unsigned char)(PEEK(0xd031) & 0xf7)); /* V400 off */
    POKE(0xd05b, 1);                                    /* CHRYSCL = 1 (16 rasters per char) */
    POKE(0xd07b, 24);                                   /* 25 text rows */
  }

  /* Ensure hot registers remain off */
  POKE(0xd05d, (unsigned char)(PEEK(0xd05d) & 0x7f));

  /* Assert Screen RAM at Bank 1 ($10000) */
  POKE(0xd060, 0x00);
  POKE(0xd061, 0x00);
  POKE(0xd062, 0x01);
  POKE(0xd063, (unsigned char)(PEEK(0xd063) & 0xf0));

  /* Update font and palette for active emulation */
  m65_screen_set_emul(cur_emul);

  m65_screen_cls();
}

void m65_screen_cycle_res(void)
{
  if (cur_res == RES_40X25) m65_screen_set_res(RES_80X25);
  else if (cur_res == RES_80X25) m65_screen_set_res(RES_80X50);
  else m65_screen_set_res(RES_40X25);
}

unsigned char m65_screen_res(void) { return cur_res; }
unsigned char m65_screen_cols(void) { return cur_cols; }
unsigned char m65_screen_rows(void) { return cur_rows; }

void m65_screen_set_emul(unsigned char emul)
{
  cur_emul = emul;
  /* Unlock VIC-IV registers */
  POKE(0xd02f, 0x47);
  POKE(0xd02f, 0x53);

  if (cur_emul == EMUL_ANSI) {
    /* Enable Palette RAM for colors 0-15 (bit 2 of $D030) */
    POKE(0xd030, (unsigned char)(PEEK(0xd030) | 0x04));
    m65_screen_set_palette_bank(1); /* 24-bit PC VGA palette */
    m65_font_set_ansi();            /* CP437 character generator */
  } else {
    /* Disable Palette RAM for colors 0-15 -> use Commodore Palette ROM */
    POKE(0xd030, (unsigned char)(PEEK(0xd030) & ~0x04));
    m65_screen_set_palette_bank(0); /* Commodore palette */
    m65_font_set_petscii(0);        /* ROM PETSCII character generator */
  }
}

void m65_screen_toggle_emul(void)
{
  m65_screen_set_emul(cur_emul == EMUL_PETSCII ? EMUL_ANSI : EMUL_PETSCII);
}

unsigned char m65_screen_emul(void) { return cur_emul; }

void m65_screen_set_border(unsigned char col)
{
  cur_border = col;
  POKE(0xd020, cur_border);
}

void m65_screen_set_bg(unsigned char col)
{
  cur_bg = col;
  POKE(0xd021, cur_bg);
}

unsigned char m65_screen_border(void) { return cur_border; }
unsigned char m65_screen_bg(void) { return cur_bg; }
unsigned char m65_screen_orig_border(void) { return orig_border; }
unsigned char m65_screen_orig_bg(void) { return orig_bg; }

void m65_screen_cls(void)
{
  unsigned int total = (unsigned int)cur_cols * cur_rows;
  if (total == 0) return;
  lfill(SCREEN, 0x20, total);
  lfill(COLOUR_RAM, cur_text, total);
}

void m65_screen_clear_row(unsigned char row, unsigned char fill_char, unsigned char col)
{
  unsigned long off;
  if (row >= cur_rows || cur_cols == 0) return;
  off = (unsigned long)row * cur_cols;
  lfill(SCREEN + off, fill_char, cur_cols);
  lfill(COLOUR_RAM + off, col, cur_cols);
}

void m65_screen_scroll_up(unsigned char top_row, unsigned char bot_row)
{
  unsigned int len;
  unsigned long src, dst;
  if (bot_row <= top_row || bot_row >= cur_rows) return;
  len = (unsigned int)(bot_row - top_row) * cur_cols;
  if (len == 0) return;
  dst = SCREEN + (unsigned long)top_row * cur_cols;
  src = SCREEN + (unsigned long)(top_row + 1) * cur_cols;
  lcopy(src, dst, len);

  dst = COLOUR_RAM + (unsigned long)top_row * cur_cols;
  src = COLOUR_RAM + (unsigned long)(top_row + 1) * cur_cols;
  lcopy(src, dst, len);

  m65_screen_clear_row(bot_row, 0x20, cur_text);
}

void m65_screen_putc(unsigned char x, unsigned char y, unsigned char ch, unsigned char col)
{
  unsigned long off;
  if (x >= cur_cols || y >= cur_rows) return;
  off = (unsigned long)y * cur_cols + x;
  lpoke(SCREEN + off, ch);
  lpoke(COLOUR_RAM + off, col);
}

static unsigned char ascii_to_screencode(unsigned char c)
{
  if (cur_emul == EMUL_ANSI) return c; /* CP437 font is ASCII-ordered */

  if (c >= 'A' && c <= 'Z') return c; /* uppercase: 65-90 */
  if (c >= 'a' && c <= 'z') return (unsigned char)(c - 96); /* lowercase: 1-26 */
  if (c >= 0x20 && c <= 0x3f) return c; /* digits, punctuation, space */
  if (c == '@') return 0;
  if (c == '[') return 27;
  if (c == ']') return 29;
  if (c == '|') return 0x5d;
  if (c == '_') return 0x64;
  return c;
}

void m65_screen_puts(unsigned char x, unsigned char y, const char *s, unsigned char col)
{
  while (*s && x < cur_cols) {
    m65_screen_putc(x++, y, ascii_to_screencode((unsigned char)*s++), col);
  }
}

void m65_screen_cursor_enable(unsigned char enable)
{
  (void)enable;
}

void m65_screen_set_cursor(unsigned char x, unsigned char y)
{
  (void)x; (void)y;
}

void m65_screen_init(void)
{
  unsigned char r_d031, r_d07b;
  unsigned char r_rows, r_cols;

  /* Read initial text mode and color configuration before modifying registers */
  r_d031 = PEEK(0xd031);
  r_d07b = PEEK(0xd07b);
  r_rows = (r_d07b == 49 || (r_d031 & 0x08)) ? 50 : 25;
  r_cols = (r_d031 & 0x80) ? 80 : 40;

  orig_border = PEEK(0xd020) & 0x0f;
  orig_bg = PEEK(0xd021) & 0x0f;
  if (orig_bg == 0) orig_bg = 6;       /* Default to Commodore Blue if black or unset */
  if (orig_border == 0) orig_border = 6;

  if (r_cols == 40) {
    cur_res = RES_40X25;
  } else if (r_rows == 50) {
    cur_res = RES_80X50;
  } else {
    cur_res = RES_80X25;
  }

  /* Unlock VIC-IV */
  POKE(0xd02f, 0x47);
  POKE(0xd02f, 0x53);

  /* Disable hot registers */
  POKE(0xd05d, (unsigned char)(PEEK(0xd05d) & 0x7f));

  /* Drain keyboard queue */
  while (PEEK(0xd610)) POKE(0xd610, 0);

  /* Program 24-bit VGA palette into Palette Bank 1 */
  init_vga_palette();

  /* Standard user requirement: startup border and background to BLACK (0), foreground to WHITE (1) */
  cur_border = 0;
  cur_bg = 0;
  cur_text = 1;
  m65_screen_set_border(cur_border);
  m65_screen_set_bg(cur_bg);

  /* Set detected initial resolution and default emulation (PETSCII) */
  cur_emul = EMUL_PETSCII;
  m65_screen_set_res(cur_res);
}
