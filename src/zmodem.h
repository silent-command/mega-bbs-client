#ifndef ZMODEM_H
#define ZMODEM_H

#define ZMODEM_OK       0
#define ZMODEM_ERR_IO   1
#define ZMODEM_ERR_CRC  2
#define ZMODEM_ERR_ABORT 3
#define ZMODEM_ERR_TIMEOUT 4

typedef struct {
  char filename[20];
  unsigned long file_size;
  unsigned long bytes_transferred;
  unsigned int cps;
  unsigned char is_upload;
  unsigned char drive;
  unsigned char active;
  unsigned char percent;
} zmodem_status_t;

typedef void (*zmodem_progress_fn)(const zmodem_status_t *st);

void zmodem_init(zmodem_progress_fn prog_fn);
void zmodem_pushback(const unsigned char *data, unsigned int len);
unsigned char zmodem_receive(unsigned char drive);
unsigned char zmodem_send(const char *filename, unsigned char drive, unsigned long file_size);
const zmodem_status_t *zmodem_get_status(void);

#endif
