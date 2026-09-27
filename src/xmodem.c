/* XMODEM (checksum, CRC, 1K) over the telnet stream.
 *
 * Against a real BBS the first version failed at the handshake: the
 * board's "start your download now" text sat in the stream and each of
 * its bytes was counted as a failed attempt. Now only timeouts count,
 * the line is purged before each NAK, EOT is confirmed, a single CAN is
 * noise and two are an abort, 1K blocks are accepted, and each block is
 * held in bank 1 until its check passes. */
#include <string.h>
#include "xmodem.h"
#include "netutil.h"
#include "telnet.h"
#include "ui.h"
#include "platform/m65_cbmdos.h"
#include "platform/m65_far.h"

#ifdef __MEGA65__
#include "mega65/memory.h"
#else
#define PEEK(addr) 0
#endif

#define SOH    0x01
#define STX    0x02
#define EOT    0x04
#define ACK    0x06
#define NAK    0x15
#define CAN    0x18
#define POLL_C 'C'

#define TIMEOUT_FRAMES 180 /* ~3 seconds */
#define MAX_RETRIES    10
#define XFER_FAR 0x12000UL /* one block, until its check passes */

static xmodem_status_t cur_status;
static xmodem_progress_fn progress_cb = 0;
static unsigned char user_abort;

#ifdef __MEGA65__
#define file_buf ((unsigned char *)0x1400)
#else
static unsigned char file_buf[256];
#endif

static unsigned int crc_step(unsigned int crc, unsigned char b)
{
  unsigned char i;
  crc ^= (unsigned int)b << 8;
  for (i = 0; i < 8; i++)
    crc = (crc & 0x8000) ? (unsigned int)((crc << 1) ^ 0x1021) : (unsigned int)(crc << 1);
  return crc & 0xffff;                          /* the host's int is wider than 16 bits */
}

unsigned int xmodem_calc_crc16(const unsigned char *buf, unsigned int len)
{
  unsigned int crc = 0;
  while (len--) crc = crc_step(crc, *buf++);
  return crc;
}

static unsigned char is_prg_file(const char *name)
{
  size_t len = strlen(name);
  if (len >= 4) {
    const char *ext = name + len - 4;
    if (ext[0] == '.' && (ext[1] == 'p' || ext[1] == 'P') &&
        (ext[2] == 'r' || ext[2] == 'R') && (ext[3] == 'g' || ext[3] == 'G')) {
      return 1;
    }
  }
  return 0;
}

static void update_progress(void)
{
  if (cur_status.file_size) cur_status.percent = transfer_percent(cur_status.bytes_transferred, cur_status.file_size);
  if (progress_cb) progress_cb(&cur_status);
}

/* One stream byte within timeout_frames. 0 on timeout, a dead link or a
 * keyboard abort (user_abort). */
static unsigned char rx_byte(unsigned char *out, unsigned int timeout_frames)
{
  unsigned int f = 0;
#ifdef __MEGA65__
  unsigned char last_f = PEEK(0xd7fa), wraps = 0;
  unsigned int spins = 0;
#endif

  for (;;) {
    unsigned char k;
    if (telnet_rx_byte(out)) return 1;
    if (!net_alive()) return 0;
    net_poll();
    k = ui_key();
    if (k == KEY_STOP || k == KEY_ESC || k == 0x03 || k == 0x18) { user_abort = 1; return 0; }
#ifdef __MEGA65__
    if (PEEK(0xd7fa) != last_f) {
      last_f = PEEK(0xd7fa);
      if (++f >= timeout_frames) return 0;
    }
    if (++spins == 0 && ++wraps >= 20) return 0;
#else
    if (++f >= timeout_frames) return 0;
#endif
  }
}

static void tx_byte(unsigned char b)
{
  telnet_tx_data(&b, 1);
}

static void tx_cancel(void)
{
  static const unsigned char cans[3] = { CAN, CAN, CAN };
  telnet_tx_data(cans, 3);
}

/* Reads until the line has been quiet for a quarter second: the classic
 * purge before a NAK, and what clears a board's prompt text before the
 * handshake. Bounded so a chattering peer cannot hold it. */
static void drain_rx(void)
{
  unsigned char b;
  unsigned int n = 0;
  while (rx_byte(&b, 15) && ++n < 4096) ;
}

static void begin(const char *filename, unsigned char drive, unsigned char is_upload)
{
  memset(&cur_status, 0, sizeof(cur_status));
  strncpy(cur_status.filename, filename, sizeof(cur_status.filename) - 1);
  cur_status.drive = drive;
  cur_status.is_upload = is_upload;
  cur_status.active = 1;
  user_abort = 0;
  update_progress();
}

static unsigned char finish(unsigned char code)
{
  if (user_abort) code = XMODEM_ERR_ABORT;
  if (code == XMODEM_ERR_ABORT || code == XMODEM_ERR_IO || code == XMODEM_ERR_CRC || code == XMODEM_ERR_TIMEOUT)
    tx_cancel();
  cur_status.active = 0;
  update_progress();
  return code;
}

/* The checked block to disk, 254 bytes at a time through file_buf. */
static unsigned char flush_far(unsigned int n)
{
  unsigned int off = 0;
  while (off < n) {
    unsigned int k = n - off, i;
    if (k > 254) k = 254;
    far_read(XFER_FAR + off, file_buf, k);
    for (i = 0; i < k; i++)
      if (cbmdos_put(file_buf[i]) != CBMDOS_OK) return 0;
    off += k;
  }
  return 1;
}

void xmodem_init(xmodem_progress_fn prog_fn)
{
  progress_cb = prog_fn;
  memset(&cur_status, 0, sizeof(cur_status));
}

const xmodem_status_t *xmodem_get_status(void)
{
  return &cur_status;
}

unsigned char xmodem_receive(const char *filename, unsigned char drive)
{
  unsigned char expected = 1;
  unsigned char use_crc = 1;
  unsigned char poll_char = POLL_C;
  unsigned char errors = 0, tries;
  unsigned char b = 0, have = 0, eot_seen = 0, can_seen = 0;

  begin(filename, drive, 0);

  {
    unsigned char type = is_prg_file(filename) ? CBMDOS_TYPE_PRG : CBMDOS_TYPE_SEQ;
    unsigned char err = cbmdos_create_as(filename, drive, type);
    if (err == CBMDOS_ERR_EXISTS && ui_confirm_overwrite(filename)) {
      cbmdos_delete(filename, drive);
      err = cbmdos_create_as(filename, drive, type);
    }
    if (err != CBMDOS_OK) return finish(XMODEM_ERR_IO);
  }

  drain_rx();

  /* 'C' and NAK by turns every two seconds: a CRC sender starts on the
   * first 'C', a checksum-only one (most PETSCII boards) on the first
   * NAK, and neither waits long. Whatever else arrives is the board
   * still talking. */
  for (tries = 0; tries < 12 && !have; tries++) {
    use_crc = (unsigned char)(!(tries & 1));
    poll_char = use_crc ? POLL_C : NAK;
    tx_byte(poll_char);
    while (rx_byte(&b, 120)) {
      if (b == SOH || b == STX || b == EOT || b == CAN) { have = 1; break; }
    }
    if (user_abort || !net_alive()) break;
  }
  if (!have) { cbmdos_close(); return finish(XMODEM_ERR_TIMEOUT); }

  for (;;) {
    if (!have) {
      if (!rx_byte(&b, TIMEOUT_FRAMES * 2)) {
        if (user_abort || !net_alive()) break;
        if (++errors >= MAX_RETRIES) break;
        cur_status.errors = errors;
        tx_byte(NAK);
        continue;
      }
    }
    have = 0;

    if (b == EOT) {
      /* NAK the first, ACK the second: a stray 0x04 cannot end the file. */
      if (eot_seen) {
        tx_byte(ACK);
        if (cbmdos_close() != CBMDOS_OK) return finish(XMODEM_ERR_IO);
        cur_status.percent = 100;
        return finish(XMODEM_OK);
      }
      eot_seen = 1;
      tx_byte(NAK);
      continue;
    }
    if (b == CAN) {
      if (can_seen) { cbmdos_close(); user_abort = 0; return finish(XMODEM_ERR_ABORT); }
      can_seen = 1;
      continue;
    }
    can_seen = 0;
    if (b != SOH && b != STX) continue;             /* noise between blocks */
    eot_seen = 0;

    {
      unsigned int blen = (b == STX) ? 1024 : 128, i, crc = 0;
      unsigned char blk = 0, inv = 0, sum = 0, ok = 1;

      if (!rx_byte(&blk, TIMEOUT_FRAMES) || !rx_byte(&inv, TIMEOUT_FRAMES) ||
          (unsigned char)(blk + inv) != 0xff)
        ok = 0;
      for (i = 0; ok && i < blen; i++) {
        if (!rx_byte(&b, TIMEOUT_FRAMES)) { ok = 0; break; }
        far_poke(XFER_FAR + i, b);
        crc = crc_step(crc, b);
        sum = (unsigned char)(sum + b);
      }
      if (ok) {
        unsigned char c1, c2;
        if (use_crc) {
          if (!rx_byte(&c1, TIMEOUT_FRAMES) || !rx_byte(&c2, TIMEOUT_FRAMES) ||
              (((unsigned int)c1 << 8) | c2) != crc)
            ok = 0;
        } else if (!rx_byte(&c1, TIMEOUT_FRAMES) || c1 != sum) {
          ok = 0;
        }
      }

      if (ok && blk == expected) {
        if (!flush_far(blen)) { cbmdos_close(); return finish(XMODEM_ERR_IO); }
        expected++;
        cur_status.blocks++;
        cur_status.bytes_transferred += blen;
        errors = 0;
        update_progress();
        tx_byte(ACK);
      } else if (ok && blk == (unsigned char)(expected - 1)) {
        tx_byte(ACK);                                 /* our ACK was lost: same block again */
      } else {
        if (user_abort || !net_alive()) break;
        if (++errors >= MAX_RETRIES) break;
        cur_status.errors = errors;
        drain_rx();
        tx_byte(NAK);
      }
    }
  }

  cbmdos_close();
  return finish(net_alive() ? (errors >= MAX_RETRIES ? XMODEM_ERR_CRC : XMODEM_ERR_TIMEOUT) : XMODEM_ERR_IO);
}

unsigned char xmodem_send(const char *filename, unsigned char drive, unsigned long file_size)
{
  unsigned char pkt[133];
  unsigned char blk_num = 1;
  unsigned char use_crc = 0;
  unsigned char b = 0, got = 0, can_seen = 0;
  unsigned char err = 0;
  unsigned char done = 0;
  unsigned char tries;
  unsigned int buf_len = 0;
  unsigned int buf_pos = 0;

  begin(filename, drive, 1);
  cur_status.file_size = file_size;

  if (cbmdos_open_read(filename, drive) != CBMDOS_OK) return finish(XMODEM_ERR_IO);

  /* The receiver's 'C' or NAK, up to a minute; the board's text between
   * them is ignored. No purge here: the receiver may already have said
   * 'C', and it would go with the text. */
  for (tries = 0; tries < 20 && !got; tries++) {
    while (rx_byte(&b, TIMEOUT_FRAMES)) {
      if (b == POLL_C) { use_crc = 1; got = 1; break; }
      if (b == NAK) { use_crc = 0; got = 1; break; }
      if (b == CAN) {
        if (can_seen) { cbmdos_close_read(); user_abort = 0; return finish(XMODEM_ERR_ABORT); }
        can_seen = 1;
      } else can_seen = 0;
    }
    if (user_abort || !net_alive()) break;
  }
  if (!got) { cbmdos_close_read(); return finish(XMODEM_ERR_TIMEOUT); }

  while (!done) {
    unsigned int n = 0, pkt_len;

    while (n < 128) {
      if (buf_pos >= buf_len) {
        buf_len = cbmdos_read_next(file_buf, &err);
        buf_pos = 0;
        if (buf_len == 0) { done = 1; break; }
      }
      pkt[3 + n++] = file_buf[buf_pos++];
    }
    if (n == 0) break;
    while (n < 128) pkt[3 + n++] = 0x1a;

    pkt[0] = SOH;
    pkt[1] = blk_num;
    pkt[2] = (unsigned char)~blk_num;
    pkt_len = 3 + 128;
    if (use_crc) {
      unsigned int crc = xmodem_calc_crc16(pkt + 3, 128);
      pkt[pkt_len++] = (unsigned char)(crc >> 8);
      pkt[pkt_len++] = (unsigned char)(crc & 0xff);
    } else {
      unsigned char sum = 0;
      unsigned int i;
      for (i = 0; i < 128; i++) sum = (unsigned char)(sum + pkt[3 + i]);
      pkt[pkt_len++] = sum;
    }

    for (tries = 0; ; tries++) {
      unsigned char resp = 0;
      if (!telnet_tx_data(pkt, pkt_len)) { cbmdos_close_read(); return finish(XMODEM_ERR_IO); }
      /* ACK or NAK; a repeated 'C' or other noise is ignored (X5) */
      while (rx_byte(&b, TIMEOUT_FRAMES * 2)) {
        if (b == ACK || b == NAK) { resp = b; break; }
        if (b == CAN) {
          if (can_seen) { cbmdos_close_read(); user_abort = 0; return finish(XMODEM_ERR_ABORT); }
          can_seen = 1;
        } else can_seen = 0;
      }
      if (user_abort || !net_alive()) { cbmdos_close_read(); return finish(XMODEM_ERR_IO); }
      if (resp == ACK) break;
      if (tries + 1 >= MAX_RETRIES) { cbmdos_close_read(); return finish(XMODEM_ERR_TIMEOUT); }
      cur_status.errors++;
    }
    blk_num++;
    cur_status.blocks++;
    cur_status.bytes_transferred += 128;
    update_progress();
  }
  cbmdos_close_read();

  /* EOT until the receiver's ACK; a NAK asks for it again. */
  got = 0;
  for (tries = 0; tries < MAX_RETRIES && !got; tries++) {
    tx_byte(EOT);
    while (rx_byte(&b, TIMEOUT_FRAMES)) {
      if (b == ACK) { got = 1; break; }
      if (b == NAK) break;
    }
    if (user_abort || !net_alive()) break;
  }
  if (!got) return finish(XMODEM_ERR_TIMEOUT);
  cur_status.percent = 100;
  return finish(XMODEM_OK);
}
