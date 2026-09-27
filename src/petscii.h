#ifndef PETSCII_H
#define PETSCII_H

void petscii_init(void);
void petscii_reset(void);
void petscii_putc(unsigned char c);
void petscii_write(const unsigned char *buf, unsigned int len);
void petscii_flush(void);                                          /* buffered text to the screen, cursor placed */
void petscii_set_window(unsigned char cols, unsigned char xoff);  /* 40 columns in the middle of 80; 0 for the whole screen */
void petscii_get_cursor(unsigned char *x, unsigned char *y);
void petscii_set_cursor(unsigned char x, unsigned char y);

unsigned char petscii_to_screencode(unsigned char p);

#endif
