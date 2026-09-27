#ifndef TELNET_H
#define TELNET_H

#define TELNET_IAC   255
#define TELNET_DONT  254
#define TELNET_DO    253
#define TELNET_WONT  252
#define TELNET_WILL  251
#define TELNET_SB    250
#define TELNET_GA    249
#define TELNET_EL    248
#define TELNET_EC    247
#define TELNET_AYT   246
#define TELNET_AO    245
#define TELNET_IP    244
#define TELNET_BRK   243
#define TELNET_DM    242
#define TELNET_NOP   241
#define TELNET_SE    240

#define TELOPT_BINARY 0
#define TELOPT_ECHO   1
#define TELOPT_SGA    3
#define TELOPT_TTYPE  24
#define TELOPT_NAWS   31

typedef void (*telnet_output_fn)(unsigned char c);
typedef void (*telnet_send_fn)(const unsigned char *data, unsigned int len);

void telnet_init(telnet_output_fn out_fn, telnet_send_fn send_fn);
void telnet_reset(void);
void telnet_send_init_negotiation(void);
void telnet_feed(const unsigned char *data, unsigned int len);
void telnet_send_naws(unsigned char cols, unsigned char rows);
void telnet_set_window_size(unsigned char cols, unsigned char rows);
void telnet_set_terminal_type(const char *name);

/* ZModem auto-detect: set when the sender's ZRQINIT went past; the bytes
 * read after it wait for the transfer. */
unsigned char telnet_check_zmodem(void);
void telnet_clear_zmodem(void);
/* The stream has paused: bytes held back as a possible header go to the screen. */
void telnet_idle(void);

/* The stream as a transfer sees it: telnet escaping undone on the way in,
 * applied on the way out. telnet_binary says whether both sides agreed to
 * BINARY (informational; IAC is doubled regardless). */
unsigned char telnet_rx_byte(unsigned char *out);
unsigned char telnet_tx_data(const unsigned char *p, unsigned int n);
unsigned char telnet_binary(void);
/* 1 once the peer has sent a telnet command; until then the link is raw. */
unsigned char telnet_is_telnet(void);

#endif
