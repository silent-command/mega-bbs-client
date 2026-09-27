#include "mega65/memory.h"
#include "meganet.h"
#include "m65_boot.h"
#include "netutil.h"

/* Three attempts of eight seconds; mega-net retries DISCOVER itself every
 * four, so this restarts a machine that gave up behind a link still
 * coming up at power-on. */
#define DHCP_ATTEMPTS 3
#define DHCP_WAIT_FRAMES 400
#define DNS_WAIT_FRAMES 400
#define CONNECT_WAIT_FRAMES 750

unsigned int net_frames;
unsigned char net_ready;
static unsigned char last_frame;

void net_poll(void)
{
  unsigned char f;
  if (net_ready) meganet_poll();
  f = PEEK(0xd7fa);
  if (f != last_frame) { last_frame = f; net_frames++; }
}

unsigned char net_load(const char **err)
{
  unsigned char attempt;

  /* Hold the ethernet controller in reset before the first call, as the
   * exit path does: a stray event left in flight by a previous program,
   * arriving during INIT's memory clear, crashes the CPU into mega-net
   * (ssh 5.8, 5.19; PLATFORM.md trap 1). INIT brings it up. */
  POKE(0xd6e0, 0x00);
  {
    unsigned char last = PEEK(0xd7fa), n = 0, wraps = 0;
    unsigned int spins = 0;
    while (n < 3) {
      if (PEEK(0xd7fa) != last) { last = PEEK(0xd7fa); n++; }
      if (++spins == 0 && ++wraps >= 20) break;   /* frame counter stalled */
    }
  }

  lpoke(0x15FFUL, 5);
  for (attempt = 0; attempt < 3; attempt++)
    if (m65_boot_load(err)) break;
  if (attempt == 3) return 0;
  lpoke(0x15FFUL, 6);
  meganet_set_restore_map(0x00, 0xE0, 0x00, 0x00);   /* the trampoline just copied carries the KERNAL map: ours again */
  meganet_call(MEGANET_INIT, 0, 0, 0, 0);
  lpoke(0x15FFUL, 7);
  net_ready = 1;
  *err = 0;
  return 1;
}

unsigned char net_dhcp(const char **err)
{
  unsigned char attempt, st;

  if (meganet_dhcp_state() == MEGANET_DHCP_BOUND) { *err = 0; return 1; }
  for (attempt = 0; attempt < DHCP_ATTEMPTS; attempt++) {
    meganet_dhcp_start();
    net_frames = 0;
    while (net_frames < DHCP_WAIT_FRAMES) {
      net_poll();
      st = meganet_dhcp_state();
      if (st == MEGANET_DHCP_BOUND) { *err = 0; return 1; }
      if (st == MEGANET_DHCP_FAILED) break;
    }
  }
  *err = "no DHCP lease (cable? router?)";
  return 0;
}

static unsigned char parse_dotted_quad(const char *s, unsigned char *out)
{
  unsigned int octet;
  unsigned char part, digits;
  for (part = 0; part < 4; part++) {
    octet = 0; digits = 0;
    while (*s >= '0' && *s <= '9') {
      octet = octet * 10 + (unsigned char)(*s - '0');
      if (octet > 255) return 0;
      digits++; s++;
    }
    if (!digits) return 0;
    out[part] = (unsigned char)octet;
    if (part < 3) { if (*s != '.') return 0; s++; }
  }
  return *s == 0;
}

unsigned char net_resolve(const char *host, unsigned char *ip, const char **err)
{
  unsigned char state;
  if (parse_dotted_quad(host, ip)) { *err = 0; return 1; }
  meganet_dns_start(host);
  net_frames = 0;
  state = MEGANET_DNS_WAITING;
  while (net_frames < DNS_WAIT_FRAMES) {
    net_poll();
    state = meganet_dns_state();
    if (state != MEGANET_DNS_WAITING) break;
  }
  if (state != MEGANET_DNS_DONE) {
    *err = (state == MEGANET_DNS_FAILED) ? "host not found" : "no reply from the name server";
    return 0;
  }
  meganet_dns_result(ip);
  *err = 0;
  return 1;
}

void net_abort(void)
{
  unsigned char i;
  meganet_tcp_abort();
  for (i = 0; i < 5; i++) net_poll();
}

unsigned char net_connect(const unsigned char *ip, unsigned int port, const char **err)
{
  unsigned char st, flags;
  net_abort();                             /* whatever socket 0 was doing, drain abort */
  meganet_tcp_connect(ip, port);
  net_frames = 0;
  while (net_frames < CONNECT_WAIT_FRAMES) {
    net_poll();
    st = meganet_tcp_state(&flags, 0);
    if (st == MEGANET_TCP_ESTABLISHED) { *err = 0; return 1; }
    if (st == MEGANET_TCP_CLOSED) {
      *err = (flags & MEGANET_TCP_F_REFUSED) ? "connection refused" : (flags & MEGANET_TCP_F_RESET) ? "connection reset" : "no route to the host";
      return 0;
    }
  }
  net_abort();
  *err = "the host did not answer";
  return 0;
}

unsigned int net_send(const unsigned char *p, unsigned int n) { return meganet_tcp_send(p, n); }
unsigned int net_recv(unsigned char *buf, unsigned int cap) { return meganet_tcp_recv(buf, cap); }

unsigned char net_alive(void)
{
  unsigned char flags;
  unsigned int avail;
  unsigned char st = meganet_tcp_state(&flags, &avail);
  if (avail) return 1;
  if (st == MEGANET_TCP_CLOSED) return 0;
  return (unsigned char)!(flags & MEGANET_TCP_F_EOF);
}

void net_close(void) { meganet_tcp_close(); }
