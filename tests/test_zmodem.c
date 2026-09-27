/* The ZMODEM module against a scripted peer that behaves like lrzsz: it
 * streams subpackets as the client consumes them, answers headers, checks
 * every CRC it receives, refuses escapes a real receiver refuses, doubles
 * 0xFF like a telnet server, and can corrupt a subpacket to provoke a
 * ZRPOS. net_send takes at most 100 bytes per call until the next poll,
 * the shape of the real stack's send buffer. */
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "zmodem.h"
#include "telnet.h"
#include "netutil.h"
#include "platform/m65_cbmdos.h"

#define ZPAD '*'
#define ZDLE 0x18
#define ZCRCE 'h'
#define ZCRCG 'i'
#define ZCRCQ 'j'
#define ZCRCW 'k'
#define ZRQINIT 0
#define ZRINIT 1
#define ZACK 3
#define ZFILE 4
#define ZSKIP 5
#define ZNAK 6
#define ZFIN 8
#define ZRPOS 9
#define ZDATA 10
#define ZEOF 11

#define FILE_LEN 20480u
#define Q_SIZE 262144u

/* ---- the wire ----------------------------------------------------------- */

static unsigned char rx_q[Q_SIZE];              /* peer to client */
static unsigned int rx_head, rx_tail;
static unsigned char tx_log[Q_SIZE];            /* client to peer, as sent */
static unsigned int tx_len;
static unsigned int send_cap = 100;
static unsigned long polls;
static unsigned long abort_at_poll;             /* ui_key says ESC from here */

static void wire_put(unsigned char b)
{
  assert(rx_tail < Q_SIZE);
  rx_q[rx_tail++] = b;
}

/* Everything the peer sends is doubled where a telnet server would;
 * a raw board doubles nothing. */
static unsigned char peer_raw;
static void wire_out(const unsigned char *p, unsigned int n)
{
  while (n--) {
    if (*p == 0xff && !peer_raw) wire_put(0xff);
    wire_put(*p++);
  }
}

static void wire_reset(void)
{
  rx_head = rx_tail = 0;
  tx_len = 0;
  polls = 0;
  abort_at_poll = 0;
}

/* ---- CRCs ---------------------------------------------------------------- */

static unsigned int crc16(unsigned int crc, unsigned char b)
{
  unsigned char i;
  crc ^= (unsigned int)b << 8;
  for (i = 0; i < 8; i++) crc = (crc & 0x8000) ? ((crc << 1) ^ 0x1021) & 0xffff : (crc << 1) & 0xffff;
  return crc;
}

static unsigned long crc32(unsigned long crc, unsigned char b)
{
  unsigned char i;
  crc ^= b;
  for (i = 0; i < 8; i++) crc = (crc & 1) ? (crc >> 1) ^ 0xedb88320UL : crc >> 1;
  return crc;
}

/* ---- the peer ------------------------------------------------------------- */

static unsigned char file_data[FILE_LEN];       /* what the peer sends, or what the client reads */

static struct {
  unsigned char role;                           /* 1: the peer sends, 0: the peer receives */
  unsigned char bin32;                          /* binary CRC-32 headers and subpackets */
  unsigned char nothing;                        /* sender with nothing to send: ZFIN at once */
  unsigned char skip;                           /* receiver that answers ZFILE with ZSKIP */
  unsigned char silent;                         /* never answers */
  int inject_bad;                               /* subpacket index to corrupt once, -1 none */
  unsigned char injected;
  unsigned long pos;                            /* sender: next byte to send */
  unsigned char streaming, eof_sent;
  unsigned int sp_index;
  /* what the peer saw */
  unsigned int hdr_count[16];
  unsigned long last_flags[16];
  unsigned long rinit_flags;
  unsigned char got_oo;
  unsigned char bad_escape;
  unsigned char crc_errors;
  unsigned char recv[FILE_LEN + 2048];          /* receiver: decoded data */
  unsigned long recv_len;
  char recv_name[64];
  unsigned long recv_size;
} peer;

static void peer_hex_header(unsigned char type, unsigned long flags)
{
  static const char hx[] = "0123456789abcdef";
  unsigned char h[5], buf[24], i, n = 0;
  unsigned int crc = 0;
  h[0] = type; h[1] = flags & 0xff; h[2] = (flags >> 8) & 0xff; h[3] = (flags >> 16) & 0xff; h[4] = (flags >> 24) & 0xff;
  buf[n++] = ZPAD; buf[n++] = ZPAD; buf[n++] = ZDLE; buf[n++] = 'B';
  for (i = 0; i < 5; i++) { crc = crc16(crc, h[i]); buf[n++] = hx[h[i] >> 4]; buf[n++] = hx[h[i] & 15]; }
  buf[n++] = hx[crc >> 12]; buf[n++] = hx[(crc >> 8) & 15]; buf[n++] = hx[(crc >> 4) & 15]; buf[n++] = hx[crc & 15];
  buf[n++] = 13; buf[n++] = 10;
  if (type != ZACK && type != ZFIN) buf[n++] = 0x11;
  wire_out(buf, n);
}

static void peer_escaped(unsigned char b)
{
  unsigned char seq[2] = { ZDLE, 0 };
  if (b == 0xff) seq[1] = 'm';
  else if (b == 0x7f) seq[1] = 'l';
  else if (b == ZDLE || b == 0x11 || b == 0x13 || b == 0x91 || b == 0x93) seq[1] = b ^ 0x40;
  else { wire_out(&b, 1); return; }
  wire_out(seq, 2);
}

static void peer_bin32_header(unsigned char type, unsigned long flags)
{
  unsigned char h[5], i, pre[3] = { ZPAD, ZDLE, 'C' };
  unsigned long c = 0xffffffffUL;
  h[0] = type; h[1] = flags & 0xff; h[2] = (flags >> 8) & 0xff; h[3] = (flags >> 16) & 0xff; h[4] = (flags >> 24) & 0xff;
  wire_out(pre, 3);
  for (i = 0; i < 5; i++) { c = crc32(c, h[i]); peer_escaped(h[i]); }
  c ^= 0xffffffffUL;
  for (i = 0; i < 4; i++) peer_escaped((unsigned char)(c >> (8 * i)));
}

static void peer_header(unsigned char type, unsigned long flags)
{
  if (peer.bin32) peer_bin32_header(type, flags);
  else peer_hex_header(type, flags);
}

/* A subpacket of n bytes ending in delim; corrupt makes one byte wrong. */
static void peer_subpacket(const unsigned char *p, unsigned int n, unsigned char delim, unsigned char corrupt)
{
  unsigned int i, c16 = 0;
  unsigned long c32 = 0xffffffffUL;
  unsigned char seq[2] = { ZDLE, delim };
  for (i = 0; i < n; i++) {
    unsigned char b = p[i];
    if (peer.bin32) c32 = crc32(c32, b); else c16 = crc16(c16, b);
    if (corrupt && i == n / 2) b ^= 0x55;
    peer_escaped(b);
  }
  wire_out(seq, 2);
  if (peer.bin32) {
    c32 = crc32(c32, delim) ^ 0xffffffffUL;
    for (i = 0; i < 4; i++) peer_escaped((unsigned char)(c32 >> (8 * i)));
  } else {
    c16 = crc16(c16, delim);
    peer_escaped(c16 >> 8);
    peer_escaped(c16 & 0xff);
  }
}

static void peer_send_zfile(void)
{
  static const unsigned char info[] = "TEST20K.BIN\0" "20480 0";
  peer_header(ZFILE, 0);
  peer_subpacket(info, sizeof(info) - 1, ZCRCW, 0);
}

/* Called when the client wants a byte and none is queued: a streaming
 * sender produces the next subpacket, like sz between reads. */
static void peer_pump(void)
{
  if (!peer.role || !peer.streaming) return;
  if (peer.pos < FILE_LEN) {
    unsigned int n = FILE_LEN - peer.pos;
    unsigned char corrupt = 0;
    if (n > 1024) n = 1024;
    if (peer.inject_bad >= 0 && !peer.injected && (int)peer.sp_index == peer.inject_bad) { corrupt = 1; peer.injected = 1; }
    peer_subpacket(file_data + peer.pos, n, (peer.pos + n == FILE_LEN) ? ZCRCE : ZCRCG, corrupt);
    peer.pos += n;
    peer.sp_index++;
  } else {
    peer_header(ZEOF, FILE_LEN);
    peer.streaming = 0;
    peer.eof_sent = 1;
  }
}

static void peer_on_header(unsigned char type, unsigned long flags)
{
  peer.hdr_count[type & 15]++;
  peer.last_flags[type & 15] = flags;
  if (peer.silent) return;
  if (peer.role) {                              /* the peer sends */
    switch (type) {
    case ZRINIT:
      peer.rinit_flags = flags;
      if (peer.nothing || peer.eof_sent) peer_header(ZFIN, 0);
      else peer_send_zfile();
      break;
    case ZNAK: peer_send_zfile(); break;
    case ZRPOS:
      peer.pos = flags;
      peer.sp_index = (unsigned int)(flags / 1024);
      peer_header(ZDATA, flags);
      peer.streaming = 1;
      break;
    case ZFIN: wire_out((const unsigned char *)"OO", 2); break;
    case ZSKIP: peer_header(ZFIN, 0); break;
    default: break;
    }
  } else {                                      /* the peer receives */
    switch (type) {
    case ZRQINIT: peer_header(ZRINIT, 0x03000400UL); break;
    case ZDATA: assert(flags == peer.recv_len); break;
    case ZEOF: assert(flags == peer.recv_len); peer_header(ZRINIT, 0x03000400UL); break;
    case ZFIN: peer_header(ZFIN, 0); wire_out((const unsigned char *)"OO", 2); break;
    default: break;
    }
  }
}

/* The receiving peer's view of a finished subpacket. */
static void peer_on_subpacket(const unsigned char *d, unsigned long n, unsigned char delim, unsigned char after_zfile)
{
  if (after_zfile) {
    unsigned int i;
    strncpy(peer.recv_name, (const char *)d, sizeof(peer.recv_name) - 1);
    i = (unsigned int)strlen(peer.recv_name) + 1;
    peer.recv_size = 0;
    while (i < n && d[i] >= '0' && d[i] <= '9') peer.recv_size = peer.recv_size * 10 + (d[i++] - '0');
    if (peer.skip) peer_header(ZSKIP, 0);
    else peer_header(ZRPOS, 0);
    return;
  }
  assert(peer.recv_len + n <= sizeof(peer.recv));
  memcpy(peer.recv + peer.recv_len, d, n);
  peer.recv_len += n;
  if (delim == ZCRCQ || delim == ZCRCW) peer_header(ZACK, peer.recv_len);
}

/* Parses the client's stream one byte at a time: telnet IAC collapsed,
 * hex headers, then subpackets after ZFILE and ZDATA. */
static struct {
  unsigned char st;                             /* 0 scan, 1 hex digits, 2 trailer, 3 subpacket */
  unsigned char scan_pos;                       /* matched "**\x18B" so far */
  char hex[15];
  unsigned char hex_n;
  unsigned char iac;
  unsigned char in_zdle;
  unsigned char sp[2048];
  unsigned long sp_n;
  unsigned char after_zfile;
  unsigned char trailer;                        /* a hex header's CR LF XON may still follow */
  unsigned char crc_pending;                    /* CRC bytes still to read after a delimiter */
  unsigned char delim;
  unsigned int crc;
  unsigned int rxcrc;
  unsigned char last_o;
} pp;

static void pp_reset(void) { memset(&pp, 0, sizeof(pp)); }

static void peer_feed_clean(unsigned char b)
{
  static const char hxs[] = "**\x18" "B";
  if (pp.st == 3) {                             /* inside a subpacket */
    unsigned char v = b, is_delim = 0;
    if (pp.trailer) {
      if (b == 13 || b == 10 || b == 0x8a || b == 0x11) return;
      pp.trailer = 0;
    }
    if (pp.in_zdle) {
      pp.in_zdle = 0;
      if (b == ZCRCE || b == ZCRCG || b == ZCRCQ || b == ZCRCW) { is_delim = 1; pp.delim = b; }
      else if (b == 'l') v = 0x7f;
      else if (b == 'm') v = 0xff;
      else if ((b & 0x60) == 0x40) v = b ^ 0x40;
      else { peer.bad_escape = 1; v = b; }
    } else if (b == ZDLE) { pp.in_zdle = 1; return; }
    else if (b == 0x11 || b == 0x13 || b == 0x91 || b == 0x93) { return; }
    if (pp.crc_pending) {
      pp.crc = crc16(pp.crc, v);
      if (--pp.crc_pending == 0) {
        if (pp.crc != 0) peer.crc_errors++;
        else peer_on_subpacket(pp.sp, pp.sp_n, pp.delim, pp.after_zfile);
        pp.sp_n = 0;
        if (pp.delim == ZCRCE || pp.delim == ZCRCW) pp.st = 0;
        pp.after_zfile = 0;
        pp.crc = 0;
      }
      return;
    }
    if (is_delim) { pp.crc = crc16(pp.crc, b); pp.crc_pending = 2; return; }
    pp.crc = crc16(pp.crc, v);
    assert(pp.sp_n < sizeof(pp.sp));
    pp.sp[pp.sp_n++] = v;
    return;
  }
  if (pp.st == 2) {                             /* hex trailer */
    if (b == 13 || b == 10 || b == 0x8a || b == 0x11) return;
    pp.st = 0;
  }
  if (pp.st == 1) {
    pp.hex[pp.hex_n++] = (char)b;
    if (pp.hex_n == 14) {
      unsigned char h[7], i;
      unsigned int crc = 0;
      unsigned long flags;
      pp.hex[14] = 0;
      for (i = 0; i < 7; i++) {
        unsigned int v;
        assert(sscanf(pp.hex + 2 * i, "%2x", &v) == 1);
        h[i] = (unsigned char)v;
        crc = crc16(crc, h[i]);
      }
      assert(crc == 0);                          /* the client's header CRC */
      flags = (unsigned long)h[1] | ((unsigned long)h[2] << 8) | ((unsigned long)h[3] << 16) | ((unsigned long)h[4] << 24);
      pp.st = 2;
      pp.hex_n = 0;
      if (!peer.role && (h[0] == ZFILE || h[0] == ZDATA)) {
        pp.st = 3;                               /* the trailer bytes are skipped inside */
        pp.trailer = 1;
        pp.after_zfile = (h[0] == ZFILE);
        pp.crc = 0;
        pp.sp_n = 0;
      }
      peer_on_header(h[0], flags);
    }
    return;
  }
  /* scanning */
  if (b == 'O') { if (pp.last_o) peer.got_oo = 1; pp.last_o = 1; } else pp.last_o = 0;
  if (b == (unsigned char)hxs[pp.scan_pos]) {
    if (++pp.scan_pos == 4) { pp.st = 1; pp.scan_pos = 0; pp.hex_n = 0; }
  } else pp.scan_pos = (b == '*') ? 1 : 0;
}

static void peer_feed(unsigned char b)
{
  if (!peer_raw) {
    if (pp.iac) { pp.iac = 0; if (b == 0xff) peer_feed_clean(0xff); return; }   /* a lone command is dropped */
    if (b == 0xff) { pp.iac = 1; return; }
  }
  peer_feed_clean(b);
}

/* ---- the mocks the module links against ---------------------------------- */

unsigned int net_send(const unsigned char *p, unsigned int n)
{
  unsigned int k = n < send_cap ? n : send_cap, i;
  send_cap -= k;
  for (i = 0; i < k; i++) {
    assert(tx_len < Q_SIZE);
    tx_log[tx_len++] = p[i];
    peer_feed(p[i]);
  }
  return k;
}

unsigned int net_recv(unsigned char *buf, unsigned int cap)
{
  (void)cap;
  if (rx_head >= rx_tail) { rx_head = rx_tail = 0; peer_pump(); }
  if (rx_head >= rx_tail) return 0;
  buf[0] = rx_q[rx_head++];
  return 1;
}

void net_poll(void)
{
  send_cap = 100;
  polls++;
}

unsigned char net_alive(void) { return 1; }

static unsigned char overwrite_answer = 1;
unsigned char ui_confirm_overwrite(const char *name) { (void)name; return overwrite_answer; }
unsigned char ui_key(void)
{
  return (abort_at_poll && polls > abort_at_poll) ? 27 : 0;
}

/* The disk: one file being written, one being read. */
static unsigned char out_file[FILE_LEN + 4096];
static unsigned long out_len;
static unsigned char file_open;
static char created_name[20];
static unsigned long src_pos;
static unsigned long put_fail_at;               /* cbmdos_put fails at this offset, 0 never */

static unsigned char exists_once, deletes;
unsigned char cbmdos_create(const char *name, unsigned char drive)
{
  (void)drive;
  if (exists_once) { exists_once = 0; return CBMDOS_ERR_EXISTS; }
  strncpy(created_name, name, sizeof(created_name) - 1);
  out_len = 0;
  file_open = 1;
  return CBMDOS_OK;
}
unsigned char cbmdos_delete(const char *name, unsigned char drive) { (void)name; (void)drive; deletes++; return CBMDOS_OK; }
unsigned char cbmdos_put(unsigned char b)
{
  if (!file_open) return CBMDOS_ERR_NOTOPEN;
  if (put_fail_at && out_len == put_fail_at) return CBMDOS_ERR_FULL;
  assert(out_len < sizeof(out_file));
  out_file[out_len++] = b;
  return CBMDOS_OK;
}
unsigned char cbmdos_close(void) { file_open = 0; return CBMDOS_OK; }
unsigned char cbmdos_open_read(const char *name, unsigned char drive) { (void)name; (void)drive; src_pos = 0; return CBMDOS_OK; }
unsigned int cbmdos_read_next(unsigned char *out, unsigned char *err)
{
  unsigned int k = FILE_LEN - src_pos;
  *err = CBMDOS_OK;
  if (k > 254) k = 254;
  memcpy(out, file_data + src_pos, k);
  src_pos += k;
  return k;
}
void cbmdos_close_read(void) {}

static unsigned int progress_calls;
static void on_progress(const zmodem_status_t *st) { (void)st; progress_calls++; }

/* ---- the cases ------------------------------------------------------------ */

static void make_file(void)
{
  unsigned long x = 12345, i;
  for (i = 0; i < FILE_LEN; i++) { x = x * 1103515245UL + 12345UL; file_data[i] = (unsigned char)(x >> 16); }
  file_data[0] = 'Z';                           /* not CR or LF: a hex header's trailer is skipped by real receivers */
  memset(file_data + 100, 0xff, 12);            /* the bytes that need escaping, in runs */
  memset(file_data + 200, 0x18, 6);
  file_data[300] = 0x11; file_data[301] = 0x13; file_data[302] = 0x91; file_data[303] = 0x93;
  file_data[400] = 0x7f;
  memset(file_data + 500, 0x04, 4);
  memset(file_data + 600, 0x1a, 8);
  memcpy(file_data + 700, "**\x18" "B00", 6);      /* looks like a ZRQINIT */
  memset(file_data + FILE_LEN - 3, 0xff, 3);     /* and at the very end */
}

static void start(unsigned char role)
{
  wire_reset();
  pp_reset();
  memset(&peer, 0, sizeof(peer));
  peer.role = role;
  peer.inject_bad = -1;
  out_len = 0;
  file_open = 0;
  put_fail_at = 0;
  progress_calls = 0;
  overwrite_answer = 1;
  exists_once = 0;
  deletes = 0;
  telnet_reset();
  peer_raw = 0;
  telnet_feed((const unsigned char *)"\xff\xfd\x03", 3);   /* IAC DO SGA at connect time: a telnet server */
}

static unsigned int count_bytes(unsigned char b)
{
  unsigned int i, n = 0;
  for (i = 0; i < tx_len; i++) if (tx_log[i] == b) n++;
  return n;
}

int main(void)
{
  unsigned char ret;

  printf("Testing ZModem...\n");
  make_file();
  zmodem_init(on_progress);

  /* Download, hex headers, CRC-16 */
  start(1);
  ret = zmodem_receive(0);
  assert(ret == ZMODEM_OK);
  assert(out_len == FILE_LEN && memcmp(out_file, file_data, FILE_LEN) == 0);
  assert(strcmp(created_name, "TEST20K.BIN") == 0);
  assert(peer.rinit_flags == 0x03000400UL);     /* 1 KB subpackets, CANFDX | CANOVIO */
  assert(peer.hdr_count[ZRPOS] == 1 && peer.last_flags[ZRPOS] == 0);
  assert(!peer.got_oo && count_bytes('O') == 0);   /* only the sender says OO */
  assert(progress_calls > 10);
  assert(zmodem_get_status()->percent == 100 && !zmodem_get_status()->active);

  /* A raw board, no telnet: nothing is doubled or collapsed */
  start(1);
  telnet_reset();
  peer_raw = 1;
  ret = zmodem_receive(0);
  assert(ret == ZMODEM_OK && out_len == FILE_LEN && memcmp(out_file, file_data, FILE_LEN) == 0);
  start(0);
  telnet_reset();
  peer_raw = 1;
  ret = zmodem_send("TEST20K.BIN", 0, FILE_LEN);
  assert(ret == ZMODEM_OK && peer.recv_len == FILE_LEN && memcmp(peer.recv, file_data, FILE_LEN) == 0);

  /* A corrupt third subpacket: one ZRPOS(2048), then a correct file */
  start(1);
  peer.inject_bad = 2;
  ret = zmodem_receive(0);
  assert(ret == ZMODEM_OK);
  assert(out_len == FILE_LEN && memcmp(out_file, file_data, FILE_LEN) == 0);
  assert(peer.hdr_count[ZRPOS] == 2 && peer.last_flags[ZRPOS] == 2048);

  /* A corrupt sixth subpacket, later in the file */
  start(1);
  peer.inject_bad = 5;
  ret = zmodem_receive(0);
  assert(ret == ZMODEM_OK && out_len == FILE_LEN && memcmp(out_file, file_data, FILE_LEN) == 0);
  assert(peer.hdr_count[ZRPOS] == 2 && peer.last_flags[ZRPOS] == 5120);

  /* The ZRQINIT arrived through the terminal: its tail is in the pushback */
  start(1);
  telnet_init(0, 0);
  telnet_feed((const unsigned char *)"rz\r**\x18" "B00000000000000\r\n\x11", 24);
  assert(telnet_check_zmodem());
  telnet_clear_zmodem();
  ret = zmodem_receive(0);
  assert(ret == ZMODEM_OK && out_len == FILE_LEN && memcmp(out_file, file_data, FILE_LEN) == 0);
  assert(peer.hdr_count[ZRINIT] >= 1);

  /* Nothing to send: ZFIN answers our ZRINIT */
  start(1);
  peer.nothing = 1;
  ret = zmodem_receive(0);
  assert(ret == ZMODEM_OK && out_len == 0);
  assert(peer.hdr_count[ZFIN] == 1);

  /* The file exists: with a yes it is replaced, with a no the sender is
   * told to skip it and nothing is written. */
  start(1);
  exists_once = 1;
  ret = zmodem_receive(0);
  assert(ret == ZMODEM_OK && deletes == 1 && out_len == FILE_LEN);
  start(1);
  exists_once = 1;
  overwrite_answer = 0;
  ret = zmodem_receive(0);
  assert(ret == ZMODEM_ERR_IO && deletes == 0 && out_len == 0);
  assert(peer.hdr_count[ZSKIP] == 1 && peer.hdr_count[ZFIN] == 1);

  /* The disk fills: the transfer is cancelled, not left hanging */
  start(1);
  put_fail_at = 3000;
  ret = zmodem_receive(0);
  assert(ret == ZMODEM_ERR_IO);
  assert(count_bytes(0x18) >= 8);              /* CAN x8 */

  /* The user presses ESC while the sender is silent */
  start(1);
  peer.silent = 1;
  abort_at_poll = 50;
  ret = zmodem_receive(0);
  assert(ret == ZMODEM_ERR_ABORT);
  assert(count_bytes(0x18) >= 8);

  /* Upload: the peer decodes what the 100-byte send cap let through */
  start(0);
  ret = zmodem_send("TEST20K.BIN", 0, FILE_LEN);
  assert(ret == ZMODEM_OK);
  assert(!peer.bad_escape && peer.crc_errors == 0);
  assert(peer.recv_len == FILE_LEN && memcmp(peer.recv, file_data, FILE_LEN) == 0);
  assert(strcmp(peer.recv_name, "TEST20K.BIN") == 0 && peer.recv_size == FILE_LEN);
  assert(peer.hdr_count[ZEOF] == 1 && peer.last_flags[ZEOF] == FILE_LEN);
  assert(peer.hdr_count[ZFIN] == 1 && peer.got_oo);
  {
    /* 0xFF went as ZDLE 'm', never as ZDLE 0xBF, and 0x18 as ZDLE 'X' */
    unsigned int i, bf = 0, m = 0, x = 0;
    for (i = 1; i < tx_len; i++) if (tx_log[i - 1] == 0x18) { if (tx_log[i] == 0xbf) bf++; if (tx_log[i] == 'm') m++; if (tx_log[i] == 'X') x++; }
    assert(bf == 0 && m >= 15 && x >= 6);
  }

  /* Upload the receiver declines */
  start(0);
  peer.skip = 1;
  ret = zmodem_send("TEST20K.BIN", 0, FILE_LEN);
  assert(ret == ZMODEM_ERR_IO);
  assert(peer.hdr_count[ZFIN] == 1 && peer.recv_len == 0);

  printf("ZModem tests passed successfully!\n");
  return 0;
}
