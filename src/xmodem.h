#ifndef XMODEM_H
#define XMODEM_H

#define XMODEM_OK          0
#define XMODEM_ERR_IO      1
#define XMODEM_ERR_CRC     2
#define XMODEM_ERR_ABORT   3
#define XMODEM_ERR_TIMEOUT 4

typedef struct {
  char filename[20];
  unsigned long file_size;
  unsigned long bytes_transferred;
  unsigned int blocks;
  unsigned int errors;
  unsigned char is_upload;
  unsigned char drive;
  unsigned char active;
  unsigned char percent;
} xmodem_status_t;

typedef void (*xmodem_progress_fn)(const xmodem_status_t *st);

void xmodem_init(xmodem_progress_fn prog_fn);
unsigned char xmodem_receive(const char *filename, unsigned char drive);
unsigned char xmodem_send(const char *filename, unsigned char drive, unsigned long file_size);
const xmodem_status_t *xmodem_get_status(void);
unsigned int xmodem_calc_crc16(const unsigned char *buf, unsigned int len);

#endif
