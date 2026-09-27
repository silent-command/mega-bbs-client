/* The XMODEM module against a scripted peer: a sender that answers 'C'
 * or NAK with blocks (128 or 1K, CRC or checksum), can corrupt a block,
 * drop a stray EOT into the stream, cancel or stay silent; and a receiver
 * that checks every packet, NAKs one on purpose, and NAKs the first EOT.
 * Both talk through a telnet-shaped wire (0xFF doubled) and the peer
 * starts by saying something, as a board does. */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "xmodem.h"
#include "telnet.h"
#include "netutil.h"
#include "platform/m65_cbmdos.h"

#define SOH 1
#define STX 2
#define EOT 4
#define ACK 6
#define NAK 0x15
#define CAN 0x18

#define Q_SIZE 262144u
#define LONG_LEN 33792u                         /* 264 blocks: the block number wraps */

static unsigned char rx_q[Q_SIZE];
static unsigned int rx_head, rx_tail;
static unsigned char tx_log[Q_SIZE];
static unsigned int tx_len;
static unsigned int send_cap = 100;
static unsigned long polls;

static unsigned char peer_raw;                  /* a plain socket: no negotiation, no doubling */
static void wire_put(unsigned char b) { assert(rx_tail < Q_SIZE); rx_q[rx_tail++] = b; }
static void wire_out(const unsigned char *p, unsigned int n)
{
  while (n--) { if (*p == 0xff && !peer_raw) wire_put(0xff); wire_put(*p++); }
}
static void wire_str(const char *s) { wire_out((const unsigned char *)s, (unsigned int)strlen(s)); }

static unsigned int crc16(const unsigned char *p, unsigned int n)
{
  unsigned int crc = 0;
  while (n--) {
    unsigned char i;
    crc ^= (unsigned int)*p++ << 8;
    for (i = 0; i < 8; i++) crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  }
  return crc;
}

static unsigned char file_data[LONG_LEN];

static struct {
  unsigned char role;                           /* 1 sends, 0 receives */
  unsigned int file_len;
  unsigned int blen;                            /* 128 or 1024 */
  unsigned char checksum;                       /* sender ignores 'C', wants NAK */
  int corrupt_block;                            /* once */
  int stray_eot_after;                          /* a lone 0x04 after this block's ACK */
  int cancel_after;                             /* CAN CAN after this block's ACK */
  unsigned char silent;
  int nak_block;                                /* receiver: NAK this block once though good */
  /* sender state */
  unsigned int next_block;                      /* 1-based index of the next block */
  unsigned char started, outstanding, sent_eot, corrupted, stray_done, cancelled, nak_done;
  unsigned int c_seen, nak_seen, ack_seen;
  /* receiver state */
  unsigned char st;                             /* 0 idle, 1 in packet */
  unsigned char pkt[1029];
  unsigned int pkt_n, pkt_len;
  unsigned char expected, eot_naked, iac, use_crc;
  unsigned char recv[LONG_LEN + 1024];
  unsigned int recv_len, packets, bad;
  unsigned char eots;
} peer;

static void peer_send_block(unsigned int idx)
{
  unsigned char hdr[3], tail[2];
  unsigned int off = (idx - 1) * peer.blen, n, i, crc;
  unsigned char data[1024], sum = 0;
  if (off >= peer.file_len) {
    unsigned char e = EOT;
    wire_out(&e, 1);
    peer.sent_eot = 1;
    return;
  }
  n = peer.file_len - off;
  if (n > peer.blen) n = peer.blen;
  memcpy(data, file_data + off, n);
  memset(data + n, 0x1a, peer.blen - n);
  hdr[0] = (peer.blen == 1024) ? STX : SOH;
  hdr[1] = (unsigned char)idx;
  hdr[2] = (unsigned char)~idx;
  wire_out(hdr, 3);
  crc = crc16(data, peer.blen);
  for (i = 0; i < peer.blen; i++) sum = (unsigned char)(sum + data[i]);
  if (peer.corrupt_block == (int)idx && !peer.corrupted) { data[peer.blen / 2] ^= 0x55; peer.corrupted = 1; }
  wire_out(data, peer.blen);
  if (peer.checksum) { wire_out(&sum, 1); }
  else { tail[0] = crc >> 8; tail[1] = crc & 0xff; wire_out(tail, 2); }
  peer.outstanding = 1;
}

static void sender_on_byte(unsigned char b)
{
  if (peer.silent) return;
  if (!peer.started) {
    if (b == 'C') peer.c_seen++;
    if (b == NAK) peer.nak_seen++;
    if ((b == 'C' && !peer.checksum) || b == NAK) {
      peer.started = 1;
      peer.next_block = 1;
      peer_send_block(peer.next_block);
    }
    return;
  }
  if (b == ACK) {
    peer.ack_seen++;
    if (peer.sent_eot) return;
    peer.outstanding = 0;
    if (peer.cancel_after == (int)peer.next_block && !peer.cancelled) {
      static const unsigned char cc[2] = { CAN, CAN };
      wire_out(cc, 2);
      peer.cancelled = 1;
      return;
    }
    if (peer.stray_eot_after == (int)peer.next_block && !peer.stray_done) {
      unsigned char e = EOT;
      wire_out(&e, 1);
      peer.stray_done = 1;
      peer.next_block++;
      return;                                   /* the block follows the client's NAK */
    }
    peer.next_block++;
    peer_send_block(peer.next_block);
  } else if (b == NAK) {
    peer.nak_seen++;
    if (peer.sent_eot) { unsigned char e = EOT; wire_out(&e, 1); }
    else peer_send_block(peer.next_block);      /* the outstanding block, or the next one */
  }
}

static void receiver_on_byte(unsigned char b)
{
  if (peer.st == 0) {
    if (b == SOH || b == STX) {
      peer.st = 1;
      peer.pkt_len = 3 + ((b == STX) ? 1024 : 128) + (peer.use_crc ? 2 : 1);
      peer.pkt[0] = b;
      peer.pkt_n = 1;
    } else if (b == EOT) {
      unsigned char r;
      peer.eots++;
      if (!peer.eot_naked) { peer.eot_naked = 1; r = NAK; } else r = ACK;
      wire_out(&r, 1);
    }
    return;
  }
  peer.pkt[peer.pkt_n++] = b;
  if (peer.pkt_n < peer.pkt_len) return;
  peer.st = 0;
  peer.packets++;
  {
    unsigned int dl = (peer.pkt[0] == STX) ? 1024 : 128, i;
    unsigned char ok = ((unsigned char)(peer.pkt[1] + peer.pkt[2]) == 0xff), r;
    if (peer.use_crc) {
      unsigned int crc = crc16(peer.pkt + 3, dl);
      if (crc != (((unsigned int)peer.pkt[3 + dl] << 8) | peer.pkt[4 + dl])) ok = 0;
    } else {
      unsigned char sum = 0;
      for (i = 0; i < dl; i++) sum = (unsigned char)(sum + peer.pkt[3 + i]);
      if (sum != peer.pkt[3 + dl]) ok = 0;
    }
    if (ok && peer.pkt[1] == peer.expected && peer.nak_block == (int)peer.expected && !peer.nak_done) {
      peer.nak_done = 1;
      ok = 0;
    }
    if (!ok) { peer.bad++; r = NAK; }
    else {
      if (peer.pkt[1] == peer.expected) {
        assert(peer.recv_len + dl <= sizeof(peer.recv));
        memcpy(peer.recv + peer.recv_len, peer.pkt + 3, dl);
        peer.recv_len += dl;
        peer.expected++;
      }
      r = ACK;
    }
    wire_out(&r, 1);
  }
}

static void peer_feed(unsigned char b)
{
  if (!peer_raw) {
    if (peer.iac) { peer.iac = 0; if (b != 0xff) return; }    /* IAC IAC is one 0xFF */
    else if (b == 0xff) { peer.iac = 1; return; }
  }
  if (peer.role) sender_on_byte(b); else receiver_on_byte(b);
}

unsigned int net_send(const unsigned char *p, unsigned int n)
{
  unsigned int k = n < send_cap ? n : send_cap, i;
  send_cap -= k;
  for (i = 0; i < k; i++) { assert(tx_len < Q_SIZE); tx_log[tx_len++] = p[i]; peer_feed(p[i]); }
  return k;
}
unsigned int net_recv(unsigned char *buf, unsigned int cap)
{
  (void)cap;
  if (rx_head >= rx_tail) { rx_head = rx_tail = 0; return 0; }
  buf[0] = rx_q[rx_head++];
  return 1;
}
void net_poll(void) { send_cap = 100; polls++; }
unsigned char net_alive(void) { return 1; }
static unsigned char overwrite_answer = 1;
unsigned char ui_confirm_overwrite(const char *name) { (void)name; return overwrite_answer; }
unsigned char ui_key(void) { return 0; }

static unsigned char out_file[LONG_LEN + 1024];
static unsigned long out_len;
static unsigned char file_open, created_prg;
static unsigned long src_pos;

unsigned char cbmdos_create(const char *name, unsigned char drive) { (void)name; (void)drive; out_len = 0; file_open = 1; created_prg = 0; return CBMDOS_OK; }
unsigned char cbmdos_create_as(const char *name, unsigned char drive, unsigned char type) { (void)name; (void)drive; out_len = 0; file_open = 1; created_prg = (type == CBMDOS_TYPE_PRG); return CBMDOS_OK; }
unsigned char cbmdos_delete(const char *name, unsigned char drive) { (void)name; (void)drive; return CBMDOS_ERR_NOTFOUND; }
unsigned char cbmdos_put(unsigned char b) { if (!file_open) return CBMDOS_ERR_NOTOPEN; assert(out_len < sizeof(out_file)); out_file[out_len++] = b; return CBMDOS_OK; }
unsigned char cbmdos_close(void) { file_open = 0; return CBMDOS_OK; }
unsigned char cbmdos_open_read(const char *name, unsigned char drive) { (void)name; (void)drive; src_pos = 0; return CBMDOS_OK; }
unsigned int cbmdos_read_next(unsigned char *out, unsigned char *err)
{
  unsigned int k = peer.file_len - src_pos;
  *err = CBMDOS_OK;
  if (k > 254) k = 254;
  memcpy(out, file_data + src_pos, k);
  src_pos += k;
  return k;
}
void cbmdos_close_read(void) {}

static void on_progress(const xmodem_status_t *st) { (void)st; }

static void start(unsigned char role, unsigned int len, unsigned int blen)
{
  rx_head = rx_tail = 0;
  tx_len = 0;
  polls = 0;
  memset(&peer, 0, sizeof(peer));
  peer.role = role;
  peer.file_len = len;
  peer.blen = blen;
  peer.corrupt_block = -1;
  peer.stray_eot_after = -1;
  peer.cancel_after = -1;
  peer.nak_block = -1;
  peer.expected = 1;
  peer.use_crc = 1;
  out_len = 0;
  file_open = 0;
  telnet_reset();
  peer_raw = 0;
  telnet_feed((const unsigned char *)"\xff\xfd\x03", 3);       /* IAC DO SGA at connect time: a telnet server */
  wire_str("Start your transfer now.\r\nWaiting...\r\n");   /* the board's prompt */
}

/* The same, from a board that is a plain socket. */
static void start_raw(unsigned char role, unsigned int len, unsigned int blen)
{
  start(role, len, blen);
  telnet_reset();
  rx_head = rx_tail = 0;
  peer_raw = 1;
  wire_str("Start your transfer now.\r\n");
}

static unsigned int count_bytes(unsigned char b)
{
  unsigned int i, n = 0;
  for (i = 0; i < tx_len; i++) if (tx_log[i] == b) n++;
  return n;
}

static unsigned int padded(unsigned int len, unsigned int blen)
{
  return ((len + blen - 1) / blen) * blen;
}

int main(void)
{
  unsigned char ret;
  unsigned long x = 99991, i;

  printf("Testing XModem engine...\n");
  for (i = 0; i < LONG_LEN; i++) { x = x * 1103515245UL + 12345UL; file_data[i] = (unsigned char)(x >> 16); }
  memset(file_data + 100, 0xff, 12);
  memset(file_data + 500, 0x04, 4);
  memset(file_data + 600, 0x18, 4);
  xmodem_init(on_progress);

  /* Test CCITT CRC-16 standard vector */
  assert(xmodem_calc_crc16((const unsigned char *)"123456789", 9) == 0x31c3);

  /* Download, 128-byte CRC blocks: the prompt text does not spend the
   * handshake, a corrupt block is NAKed once, a stray EOT does not end
   * the file, the real EOT is confirmed. */
  start(1, 20480, 128);
  peer.corrupt_block = 5;
  peer.stray_eot_after = 7;
  ret = xmodem_receive("TEST.BIN", 0);
  assert(ret == XMODEM_OK);
  assert(out_len == 20480 && memcmp(out_file, file_data, 20480) == 0);
  assert(peer.c_seen == 1 && peer.nak_seen == 3);   /* the first C was answered; the NAKs: corrupt block, stray EOT, first real EOT */
  assert(count_bytes(NAK) == 3);                /* corrupt block, stray EOT, first real EOT */
  assert(count_bytes(ACK) == 161);
  assert(xmodem_get_status()->blocks == 160 && xmodem_get_status()->percent == 100);

  /* A raw board (no telnet at all): 0xFF bytes arrive as they are */
  start_raw(1, 20480, 128);
  ret = xmodem_receive("TEST.BIN", 0);
  assert(ret == XMODEM_OK && out_len == 20480 && memcmp(out_file, file_data, 20480) == 0);
  start_raw(0, 20480, 128);
  wire_str("C");
  ret = xmodem_send("TEST.BIN", 0, 20480);
  assert(ret == XMODEM_OK && peer.recv_len == 20480 && memcmp(peer.recv, file_data, 20480) == 0);

  /* 1K blocks */
  start(1, 20480, 1024);
  ret = xmodem_receive("TEST.BIN", 0);
  assert(ret == XMODEM_OK && out_len == 20480 && memcmp(out_file, file_data, 20480) == 0);
  assert(xmodem_get_status()->blocks == 20);

  /* A checksum-only sender: four 'C's go unanswered, then NAK */
  start(1, 20480, 128);
  peer.checksum = 1;
  ret = xmodem_receive("TEST.BIN", 0);
  assert(ret == XMODEM_OK && out_len == 20480 && memcmp(out_file, file_data, 20480) == 0);
  assert(peer.c_seen == 1 && peer.nak_seen >= 1);   /* one 'C' ignored, then the NAK started it */

  /* Block numbers wrap past 255; block 0's complement is 0xFF on the wire */
  start(1, LONG_LEN, 128);
  ret = xmodem_receive("LONG.PRG", 0);
  assert(ret == XMODEM_OK && out_len == LONG_LEN && memcmp(out_file, file_data, LONG_LEN) == 0);
  assert(created_prg);

  /* A file that is not a multiple of the block: the padding stays */
  start(1, 20000, 128);
  ret = xmodem_receive("TEST.BIN", 0);
  assert(ret == XMODEM_OK && out_len == padded(20000, 128));
  assert(memcmp(out_file, file_data, 20000) == 0 && out_file[20000] == 0x1a && out_file[out_len - 1] == 0x1a);

  /* The sender cancels */
  start(1, 20480, 128);
  peer.cancel_after = 3;
  ret = xmodem_receive("TEST.BIN", 0);
  assert(ret == XMODEM_ERR_ABORT);
  assert(out_len == 3 * 128);

  /* Nobody answers: ten tries, four with 'C' and six with NAK */
  start(1, 20480, 128);
  peer.silent = 1;
  ret = xmodem_receive("TEST.BIN", 0);
  assert(ret == XMODEM_ERR_TIMEOUT);
  assert(count_bytes('C') == 6 && count_bytes(NAK) == 6);
  assert(count_bytes(CAN) >= 2);

  /* Upload: the receiver's prompt and a repeated 'C' are ignored, block
   * 4 is NAKed once and resent, the first EOT is NAKed and sent again. */
  start(0, 20480, 128);
  peer.nak_block = 4;
  wire_str("CC");
  ret = xmodem_send("TEST.BIN", 0, 20480);
  assert(ret == XMODEM_OK);
  assert(peer.recv_len == 20480 && memcmp(peer.recv, file_data, 20480) == 0);
  assert(peer.packets == 161 && peer.bad == 1 && peer.eots == 2);

  /* Upload to a checksum receiver */
  start(0, 20480, 128);
  peer.use_crc = 0;
  { unsigned char n = NAK; wire_out(&n, 1); }
  ret = xmodem_send("TEST.BIN", 0, 20480);
  assert(ret == XMODEM_OK && peer.recv_len == 20480 && memcmp(peer.recv, file_data, 20480) == 0 && peer.bad == 0);

  /* Upload of an odd length: the receiver gets the padding */
  start(0, 20000, 128);
  wire_str("C");
  ret = xmodem_send("TEST.BIN", 0, 20000);
  assert(ret == XMODEM_OK && peer.recv_len == padded(20000, 128) && memcmp(peer.recv, file_data, 20000) == 0);
  assert(peer.recv[20000] == 0x1a);

  printf("XModem tests passed successfully!\n");
  return 0;
}
