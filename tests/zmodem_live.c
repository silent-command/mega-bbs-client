/* The real transfer modules over a real TCP connection, against lrzsz
 * behind tools/lrzsz_bridge.py, the Python test server, or any telnet
 * board:
 *
 *   zmodem_live HOST PORT MODE FILE KEY PROMPT WAIT DONE [NAME]
 *
 * MODE is recv, send, xrecv or xsend; FILE the host file to write or
 * read; KEY what to type once PROMPT has been seen; WAIT the text that
 * says the peer is ready ("" for recv: the sender's ZRQINIT is awaited
 * instead); DONE the text the peer prints afterwards; NAME the name sent
 * with an upload. Exit status 0 when the module reported success. The
 * disk is a host file; the keyboard never presses anything; a poll
 * sleeps 20 ms, so the modules' frame counts come to about their
 * intended seconds. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <sys/socket.h>
#include "zmodem.h"
#include "xmodem.h"
#include "telnet.h"
#include "netutil.h"
#include "platform/m65_cbmdos.h"

static int sock = -1;
static int alive = 1;

unsigned int net_send(const unsigned char *p, unsigned int n)
{
  ssize_t k = send(sock, p, n, 0);
  if (k < 0) { if (errno == EAGAIN || errno == EWOULDBLOCK) return 0; alive = 0; return 0; }
  return (unsigned int)k;
}
unsigned int net_recv(unsigned char *buf, unsigned int cap)
{
  ssize_t k = recv(sock, buf, cap, 0);
  if (k == 0) { alive = 0; return 0; }
  if (k < 0) { if (errno != EAGAIN && errno != EWOULDBLOCK) alive = 0; return 0; }
  return (unsigned int)k;
}
void net_poll(void) { usleep(20000); }
unsigned char net_alive(void) { return (unsigned char)alive; }
unsigned char ui_confirm_overwrite(const char *name) { (void)name; return 1; }
unsigned char ui_key(void) { return 0; }

static FILE *out_fp, *in_fp;
static const char *out_path;

unsigned char cbmdos_create(const char *name, unsigned char drive)
{
  (void)drive;
  fprintf(stderr, "[disk] create %s -> %s\n", name, out_path);
  out_fp = fopen(out_path, "wb");
  return out_fp ? CBMDOS_OK : CBMDOS_ERR_IO;
}
unsigned char cbmdos_create_as(const char *name, unsigned char drive, unsigned char type) { (void)type; return cbmdos_create(name, drive); }
unsigned char cbmdos_delete(const char *name, unsigned char drive) { (void)name; (void)drive; return CBMDOS_ERR_NOTFOUND; }
unsigned char cbmdos_put(unsigned char b) { return (out_fp && fputc(b, out_fp) != EOF) ? CBMDOS_OK : CBMDOS_ERR_IO; }
unsigned char cbmdos_close(void) { if (out_fp) fclose(out_fp); out_fp = 0; return CBMDOS_OK; }
unsigned char cbmdos_open_read(const char *name, unsigned char drive) { (void)name; (void)drive; return in_fp ? CBMDOS_OK : CBMDOS_ERR_NOTFOUND; }
unsigned int cbmdos_read_next(unsigned char *out, unsigned char *err) { *err = CBMDOS_OK; return (unsigned int)fread(out, 1, 254, in_fp); }
void cbmdos_close_read(void) {}

static void zprog(const zmodem_status_t *st) { fprintf(stderr, "\r[zmodem] %s %lu/%lu %u%%   ", st->filename, st->bytes_transferred, st->file_size, st->percent); }
static void xprog(const xmodem_status_t *st) { fprintf(stderr, "\r[xmodem] %s %lu bytes, %u blocks, %u errors   ", st->filename, st->bytes_transferred, st->blocks, st->errors); }

static char seen[4096];
static unsigned int sl;
static void term_out(unsigned char c)
{
  fputc(c, stderr);
  if (sl < sizeof(seen) - 1) { seen[sl++] = (char)c; seen[sl] = 0; }
}
static void term_send(const unsigned char *d, unsigned int n) { send(sock, d, n, 0); }

/* Reads the bridge's text through the terminal path until `until` has
 * been seen or a ZRQINIT was detected (as main.c does). */
static int chat(const char *until, int stop_on_zmodem)
{
  unsigned char buf[128];
  unsigned int n;
  int i;
  sl = 0;
  seen[0] = 0;
  for (i = 0; i < 1500; i++) {
    n = net_recv(buf, sizeof(buf));
    if (n) {
      telnet_feed(buf, n);
      if (stop_on_zmodem && telnet_check_zmodem()) { telnet_clear_zmodem(); return 1; }
      if (until && until[0] && strstr(seen, until)) return 1;
    } else if (!alive) {
      return 0;
    } else {
      net_poll();
    }
  }
  return 0;
}

int main(int argc, char **argv)
{
  struct addrinfo hints, *res;
  const char *mode, *key, *prompt, *wait, *done;
  unsigned char ret = 99;

  if (argc < 9) { fprintf(stderr, "usage: see the top of zmodem_live.c\n"); return 2; }
  mode = argv[3]; key = argv[5]; prompt = argv[6]; wait = argv[7]; done = argv[8];
  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(argv[1], argv[2], &hints, &res)) { perror("getaddrinfo"); return 2; }
  sock = socket(res->ai_family, res->ai_socktype, 0);
  if (connect(sock, res->ai_addr, res->ai_addrlen)) { perror("connect"); return 2; }
  fcntl(sock, F_SETFL, O_NONBLOCK);

  telnet_init(term_out, term_send);
  telnet_send_init_negotiation();
  zmodem_init(zprog);
  xmodem_init(xprog);
  if (!chat(prompt, 0)) { fprintf(stderr, "no prompt\n"); return 3; }
  term_send((const unsigned char *)key, (unsigned int)strlen(key));

  if (!strcmp(mode, "recv")) {
    out_path = argv[4];
    if (!chat(wait, wait[0] == 0)) { fprintf(stderr, "the sender never started\n"); return 3; }
    ret = zmodem_receive(0);
  } else if (!strcmp(mode, "send")) {
    long len;
    in_fp = fopen(argv[4], "rb");
    if (!in_fp) { perror(argv[4]); return 2; }
    chat(wait, 0);
    fseek(in_fp, 0, SEEK_END); len = ftell(in_fp); fseek(in_fp, 0, SEEK_SET);
    ret = zmodem_send(argc > 9 ? argv[9] : "UPLOAD.BIN", 0, (unsigned long)len);
  } else if (!strcmp(mode, "xrecv")) {
    out_path = argv[4];
    chat(wait, 0);
    ret = xmodem_receive("XDOWN.BIN", 0);
  } else if (!strcmp(mode, "xsend")) {
    in_fp = fopen(argv[4], "rb");
    if (!in_fp) { perror(argv[4]); return 2; }
    chat(wait, 0);
    ret = xmodem_send("XUP.BIN", 0, 0);
  }
  fprintf(stderr, "\n[result] %s -> %u\n", mode, ret);
  chat(done, 0);
  term_send((const unsigned char *)"\rQ", 2);
  chat("", 0);
  close(sock);
  return ret == 0 ? 0 : 1;
}
