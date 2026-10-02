#ifndef UI_H
#define UI_H

#define KEY_NONE   0
#define KEY_STOP   3
#define KEY_TAB    9
#define KEY_RETURN 13
#define KEY_DOWN   17
#define KEY_HOME   19
#define KEY_CLR    0x93           /* SHIFT + HOME: the last site */
#define KEY_DEL    20
#define KEY_ESC    27
#define KEY_RIGHT  29
#define KEY_UP     145
#define KEY_LEFT   157

#define KEY_F1     0xf1
#define KEY_F2     0xf2
#define KEY_F3     0xf3
#define KEY_F4     0xf4
#define KEY_F5     0xf5
#define KEY_F6     0xf6
#define KEY_F7     0xf7

#define IS_KEY_F1(k) ((k) == 0xf1 || (k) == 133)
#define IS_KEY_F2(k) ((k) == 0xf2 || (k) == 137)
#define IS_KEY_F3(k) ((k) == 0xf3 || (k) == 134)
#define IS_KEY_F4(k) ((k) == 0xf4 || (k) == 138)
#define IS_KEY_F5(k) ((k) == 0xf5 || (k) == 135)
#define IS_KEY_F6(k) ((k) == 0xf6 || (k) == 139)
#define IS_KEY_F7(k) ((k) == 0xf7 || (k) == 136)

#include "bookmarks.h"
#include "zmodem.h"
#include "xmodem.h"

#define TRANSFER_ACT_NONE     0
#define TRANSFER_ACT_Z_UP     1
#define TRANSFER_ACT_Z_DOWN   2
#define TRANSFER_ACT_X_UP     3
#define TRANSFER_ACT_X_DOWN   4

#define BBS_VERSION "1.2"

/* The drive transfers read and write, 0 for 8 and 1 for 9; F5 toggles it
 * in the directory and in a session. */
extern unsigned char work_drive;
/* The menus' text color (MEGA-F cycles it) and $D611 as the last key was read. */
extern unsigned char ui_fg, ui_last_mods;
/* A row of a and b, padded to the full width, reversed when rev; the
 * status row, two above the bottom, as the other clients have it. */
void ui_row(unsigned char row, const char *a, const char *b, unsigned char rev);
void ui_status(const char *a, const char *b);

unsigned char ui_key(void);
unsigned char ui_wait_key(void);
unsigned char ui_read_line(unsigned char row, const char *prompt, char *out, unsigned char maxlen);

void ui_draw_zmodem_progress(const zmodem_status_t *st);
/* Asks before a download replaces a file of the same name; 1 for yes. */
unsigned char ui_confirm_overwrite(const char *name);
void ui_draw_xmodem_progress(const xmodem_status_t *st);

/* Transfer selection modal: returns one of TRANSFER_ACT_* */
unsigned char ui_transfer_menu(void);

/* "300".."9600" or "max" */
const char *ui_speed_name(unsigned char speed);

/* Dialing Directory: returns index to connect, or 0xff on cancel/exit */
unsigned char ui_dialing_directory(unsigned char boot_drive);
/* ui_dialing_directory's answer for a Quick-Dial: the site is
 * ui_quick_site(), which is never added to the saved list. */
#define UI_QUICK_DIAL 0xfe
bookmark_t *ui_quick_site(void);

/* Interactive File Picker for Upload: returns 1 if file selected, 0 if cancelled */
unsigned char ui_file_picker(unsigned char drive, char *out_filename, unsigned long *out_size);


#endif
