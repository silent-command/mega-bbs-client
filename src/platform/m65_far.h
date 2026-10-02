/* Tables kept in bank 1, reached through the DMA helpers.
 *
 * Bank 1 is outside the CPU's 64K map, so a table there cannot be
 * addressed through a pointer: a module keeps one near copy of an entry
 * (its window), reads an entry into it with far_read and writes it back
 * with far_write. Moving the two big tables here is what freed the soft
 * stack, which the program's data had grown into.
 *
 * Bank 1 in this client:
 *   $10000-$10F9F  screen RAM (80x50)
 *   $11000-$117FF  CP437 font copy
 *   $11C50-$11ECF  file picker listing, 32 x 20 bytes (ui.c)
 *   $12000-$123FF  transfer block buffer, 1 KB (zmodem.c, xmodem.c)
 *   $12400-$159E7  stored sites, 200 x 69 bytes (bookmarks.c); 16 at
 *                  $11800 until 2026-10-02, which was the whole cap
 *
 * A zero-length lcopy copies 64KB: every caller guards its length.
 *
 * The host build keeps a plain array behind the same three macros so the
 * accessor code in each module is the code the tests run. */
#ifndef M65_FAR_H
#define M65_FAR_H

#define FAR_BASE 0x11800UL

#ifdef __MEGA65__
#include "mega65/memory.h"
#define far_read(far, near, n)  lcopy((far), (unsigned long)(unsigned int)(near), (n))
#define far_write(near, far, n) lcopy((unsigned long)(unsigned int)(near), (far), (n))
#define far_move(src, dst, n)   lcopy((src), (dst), (n))
#define far_poke(far, v)        lpoke((far), (v))
#else
#include <string.h>
static unsigned char far_mem[0x4200] __attribute__((unused));   /* to the end of the site table */
#define far_read(far, near, n)  memcpy((near), far_mem + ((far) - FAR_BASE), (n))
#define far_write(near, far, n) memcpy(far_mem + ((far) - FAR_BASE), (near), (n))
#define far_move(src, dst, n)   memmove(far_mem + ((dst) - FAR_BASE), far_mem + ((src) - FAR_BASE), (n))
#define far_poke(far, v)        (far_mem[(far) - FAR_BASE] = (v))
#endif

#endif
