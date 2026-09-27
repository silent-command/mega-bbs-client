#include <string.h>
#include "telnet.h"
#include "netutil.h"

#define TSTATE_DATA 0
#define TSTATE_IAC  1
#define TSTATE_DO   2
#define TSTATE_DONT 3
#define TSTATE_WILL 4
#define TSTATE_WONT 5
#define TSTATE_SB   6
#define TSTATE_SB_IAC 7

static unsigned char tstate = TSTATE_DATA;
static unsigned char sb_opt = 0;
static unsigned char sb_buf[32];
static unsigned char sb_len = 0;

static telnet_output_fn client_out = 0;
static telnet_send_fn client_send = 0;

static char term_type[16] = "ANSI";
static unsigned char zmodem_detected = 0;
static unsigned char in_transfer = 0;       /* detection off while a transfer reads the stream */
static unsigned char bin_rx = 0, bin_tx = 0; /* the peer's BINARY answers */

/* Bytes the terminal had read past a detected ZRQINIT: the header itself
 * and the rest of that receive, up to 5 + 128. The transfer reads them
 * first. Lives at $1D80, clear of the F011 sector buffer ($1B00-$1CFF)
 * that disk writes go through. */
#define PUSHBACK_MAX 255
#ifdef __MEGA65__
#define pushback_buf ((unsigned char *)0x1d80)
#else
static unsigned char pushback_buf[PUSHBACK_MAX];
#endif
static unsigned char pb_len = 0, pb_pos = 0;
static unsigned char cap_byte, cap_have;      /* the byte telnet_rx_byte is collecting */

static void pushback(const unsigned char *data, unsigned int len)
{
  while (len-- && pb_len < PUSHBACK_MAX)
    pushback_buf[pb_len++] = *data++;
}

/* Sliding detector for the sender's ZRQINIT, hex "**\x18B00" or binary
 * "*\x18A\0" / "*\x18C\0". Only ZRQINIT starts a download: a remote rz
 * answering an upload sends ZRINIT ("B01"), which used to match too. */
static unsigned char zdetect_buf[8];
static unsigned char zdetect_pos = 0;

static unsigned char check_zmodem_seq(unsigned char c)
{
  unsigned char i;
  if (zdetect_pos < sizeof(zdetect_buf)) {
    zdetect_buf[zdetect_pos++] = c;
  } else {
    for (i = 0; i < sizeof(zdetect_buf) - 1; i++) {
      zdetect_buf[i] = zdetect_buf[i + 1];
    }
    zdetect_buf[sizeof(zdetect_buf) - 1] = c;
  }

  /* Binary ZRQINIT: "*" ZDLE ZBIN/ZBIN32, type byte 0 */
  if (zdetect_pos >= 4) {
    unsigned char p = (unsigned char)(zdetect_pos - 4);
    if (zdetect_buf[p] == '*' && zdetect_buf[p + 1] == 0x18 &&
        (zdetect_buf[p + 2] == 'C' || zdetect_buf[p + 2] == 'A') &&
        zdetect_buf[p + 3] == 0) {
      zmodem_detected = 1;
      return 4;
    }
  }

  /* Hex ZRQINIT: "**" ZDLE "B00" */
  if (zdetect_pos >= 6) {
    unsigned char p = (unsigned char)(zdetect_pos - 6);
    if (zdetect_buf[p] == '*' && zdetect_buf[p + 1] == '*' &&
        zdetect_buf[p + 2] == 0x18 && zdetect_buf[p + 3] == 'B' &&
        zdetect_buf[p + 4] == '0' && zdetect_buf[p + 5] == '0') {
      zmodem_detected = 1;
      return 6;
    }
  }

  return 0;
}

unsigned char telnet_check_zmodem(void)
{
  return zmodem_detected;
}

void telnet_clear_zmodem(void)
{
  zmodem_detected = 0;
  zdetect_pos = 0;
}

void telnet_init(telnet_output_fn out_fn, telnet_send_fn send_fn)
{
  client_out = out_fn;
  client_send = send_fn;
  telnet_reset();
}

void telnet_reset(void)
{
  tstate = TSTATE_DATA;
  sb_opt = 0;
  sb_len = 0;
  zmodem_detected = 0;
  zdetect_pos = 0;
  in_transfer = 0;
  bin_rx = 0;
  bin_tx = 0;
  pb_len = 0;
  pb_pos = 0;
}

unsigned char telnet_binary(void)
{
  return (unsigned char)(bin_rx && bin_tx);
}

static unsigned char win_cols = 80;
static unsigned char win_rows = 25;

void telnet_set_window_size(unsigned char cols, unsigned char rows)
{
  win_cols = cols;
  win_rows = rows;
}

void telnet_set_terminal_type(const char *name)
{
  strncpy(term_type, name, sizeof(term_type) - 1);
  term_type[sizeof(term_type) - 1] = 0;
}

static void send_reply(unsigned char cmd, unsigned char opt)
{
  unsigned char buf[3];
  buf[0] = TELNET_IAC;
  buf[1] = cmd;
  buf[2] = opt;
  if (client_send) client_send(buf, 3);
}

/* BINARY both ways is asked for up front so a server does not translate
 * CR or NUL in transfer data; IAC stays doubled either way. */
void telnet_send_init_negotiation(void)
{
  unsigned char buf[18];
  buf[0] = TELNET_IAC; buf[1] = TELNET_WILL; buf[2] = TELOPT_SGA;
  buf[3] = TELNET_IAC; buf[4] = TELNET_DO;   buf[5] = TELOPT_SGA;
  buf[6] = TELNET_IAC; buf[7] = TELNET_WILL; buf[8] = TELOPT_TTYPE;
  buf[9] = TELNET_IAC; buf[10] = TELNET_WILL; buf[11] = TELOPT_NAWS;
  buf[12] = TELNET_IAC; buf[13] = TELNET_WILL; buf[14] = TELOPT_BINARY;
  buf[15] = TELNET_IAC; buf[16] = TELNET_DO;   buf[17] = TELOPT_BINARY;
  if (client_send) client_send(buf, 18);
}

void telnet_send_naws(unsigned char cols, unsigned char rows)
{
  unsigned char buf[9];
  buf[0] = TELNET_IAC;
  buf[1] = TELNET_SB;
  buf[2] = TELOPT_NAWS;
  buf[3] = 0;
  buf[4] = cols;
  buf[5] = 0;
  buf[6] = rows;
  buf[7] = TELNET_IAC;
  buf[8] = TELNET_SE;
  if (client_send) client_send(buf, 9);
}

static void handle_subnegotiation(void)
{
  if (sb_opt == TELOPT_TTYPE && sb_len >= 1 && sb_buf[0] == 1) {
    /* SEND terminal type: respond with IS <term_type> */
    unsigned char reply[32];
    unsigned char len = 0;
    unsigned char n = (unsigned char)strlen(term_type);

    reply[len++] = TELNET_IAC;
    reply[len++] = TELNET_SB;
    reply[len++] = TELOPT_TTYPE;
    reply[len++] = 0; /* IS */
    if (n > 20) n = 20;
    memcpy(reply + len, term_type, n);
    len += n;
    reply[len++] = TELNET_IAC;
    reply[len++] = TELNET_SE;

    if (client_send) client_send(reply, len);
  }
}

void telnet_feed(const unsigned char *data, unsigned int len)
{
  while (len--) {
    unsigned char c = *data++;

    switch (tstate) {
    case TSTATE_DATA:
      if (c == TELNET_IAC) {
        tstate = TSTATE_IAC;
      } else if (in_transfer) {
        cap_byte = c;
        cap_have = 1;
      } else {
        unsigned char m = check_zmodem_seq(c);
        if (m) {
          pushback(&zdetect_buf[zdetect_pos - m], m);
          pushback(data, len);
          return;
        }
        if (client_out) client_out(c);
      }
      break;

    case TSTATE_IAC:
      if (c == TELNET_IAC) {
        /* Literal 255 */
        if (in_transfer) { cap_byte = c; cap_have = 1; }
        else if (client_out) client_out(c);
        tstate = TSTATE_DATA;
      } else if (c == TELNET_DO) {
        tstate = TSTATE_DO;
      } else if (c == TELNET_DONT) {
        tstate = TSTATE_DONT;
      } else if (c == TELNET_WILL) {
        tstate = TSTATE_WILL;
      } else if (c == TELNET_WONT) {
        tstate = TSTATE_WONT;
      } else if (c == TELNET_SB) {
        tstate = TSTATE_SB;
        sb_opt = 0;
        sb_len = 0;
      } else {
        /* Single-byte commands: GA, NOP, SE, etc. swallowed cleanly */
        tstate = TSTATE_DATA;
      }
      break;

    case TSTATE_DO:
      if (c == TELOPT_BINARY || c == TELOPT_TTYPE || c == TELOPT_NAWS || c == TELOPT_SGA) {
        send_reply(TELNET_WILL, c);
        if (c == TELOPT_NAWS) {
          telnet_send_naws(win_cols, win_rows);
        }
        if (c == TELOPT_BINARY) bin_tx = 1;
      } else {
        send_reply(TELNET_WONT, c);
      }
      tstate = TSTATE_DATA;
      break;

    case TSTATE_DONT:
      send_reply(TELNET_WONT, c);
      if (c == TELOPT_BINARY) bin_tx = 0;
      tstate = TSTATE_DATA;
      break;

    case TSTATE_WILL:
      if (c == TELOPT_BINARY || c == TELOPT_ECHO || c == TELOPT_SGA) {
        send_reply(TELNET_DO, c);
        if (c == TELOPT_BINARY) bin_rx = 1;
      } else {
        send_reply(TELNET_DONT, c);
      }
      tstate = TSTATE_DATA;
      break;

    case TSTATE_WONT:
      send_reply(TELNET_DONT, c);
      if (c == TELOPT_BINARY) bin_rx = 0;
      tstate = TSTATE_DATA;
      break;

    case TSTATE_SB:
      if (sb_opt == 0) {
        sb_opt = c;
      } else if (c == TELNET_IAC) {
        tstate = TSTATE_SB_IAC;
      } else {
        if (sb_len < sizeof(sb_buf)) sb_buf[sb_len++] = c;
      }
      break;

    case TSTATE_SB_IAC:
      if (c == TELNET_SE) {
        handle_subnegotiation();
        tstate = TSTATE_DATA;
      } else if (c == TELNET_IAC) {
        if (sb_len < sizeof(sb_buf)) sb_buf[sb_len++] = c;
        tstate = TSTATE_SB;
      } else {
        tstate = TSTATE_DATA;
      }
      break;
    }
  }
}

/* ---- the stream during a transfer ------------------------------------- */

/* One data byte for a transfer, with telnet's escaping undone: IAC IAC is
 * one 0xFF and commands are answered or skipped by the same state machine
 * the terminal uses, so an option request arriving mid-transfer is handled
 * rather than written to the file. Pushed-back bytes come first. Returns 0
 * when no byte is ready. */
unsigned char telnet_rx_byte(unsigned char *out)
{
  unsigned char c;

  cap_have = 0;
  in_transfer = 1;
  while (!cap_have) {
    if (pb_pos < pb_len) {
      c = pushback_buf[pb_pos++];
      if (pb_pos == pb_len) pb_pos = pb_len = 0;
    } else if (net_recv(&c, 1) != 1) {
      break;
    }
    telnet_feed(&c, 1);
  }
  in_transfer = 0;
  *out = cap_byte;
  return cap_have;
}

/* Sends transfer bytes with every 0xFF doubled: each run is sent up to
 * and including its 0xFF, and the next run starts at that same byte.
 * Returns 0 if the link failed. */
unsigned char telnet_tx_data(const unsigned char *p, unsigned int n)
{
  const unsigned char *s = p;
  while (n--) {
    if (*p == TELNET_IAC) {
      if (!net_send_all(s, (unsigned int)(p - s + 1))) return 0;
      s = p;
    }
    p++;
  }
  return net_send_all(s, (unsigned int)(p - s));
}
