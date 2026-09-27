#ifndef ANSI_H
#define ANSI_H

typedef void (*ansi_response_fn)(const unsigned char *data, unsigned int len);

void ansi_init(void);
void ansi_reset(void);
void ansi_set_response_fn(ansi_response_fn fn);
void ansi_putc(unsigned char c);
void ansi_write(const unsigned char *buf, unsigned int len);
void ansi_get_cursor(unsigned char *x, unsigned char *y);
void ansi_set_cursor(unsigned char x, unsigned char y);

#endif
