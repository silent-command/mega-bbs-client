/* Fixed-address working storage below the load address ($2001).
 *
 * Low RAM in this client (verified free on hardware by pattern writes
 * that survived DHCP, a session and a transfer):
 *
 *   $1400-$14FF  bookmarks.c load buffer / xmodem.c file_buf / zmodem.c cur_buf
 *   $1500-$15FE  bookmarks.c load line / zmodem.c next_buf   ($15FF: boot crumb)
 *   $1600-$16FF  mega-net trampoline (reserved)
 *   $1700-$17FF  spare
 *   $1800-$18FF  CBM DOS BAM
 *   $1900-$19FF  CBM DOS block buffer
 *   $1A00-$1AFF  CBM DOS second BAM sector
 *   $1B00-$1CFF  F011 sector buffer
 *   $1D00-$1D7F  main.c rx_buf
 *   $1D80-$1E7F  zmodem.c pushback buffer
 *   $1E80-$1FAF  spare
 *   $1FB0-$1FFF  exit stub
 *
 * The aliases at $1400/$1500 are safe because the dialing directory and a
 * file transfer never run at the same time. Larger tables live in bank 1
 * (m65_far.h). */
#ifndef M65_SCRATCH_H
#define M65_SCRATCH_H

/* The upper-cased, NUL-padded file name the CBM DOS layer works on. An
 * ordinary .bss array: a fixed address at $1000 once collided with BASIC's
 * function-key table. */
extern char scr_dos_name[17];
#define SCR_DOS_NAME scr_dos_name

#endif
