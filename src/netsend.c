/* Sending a whole buffer through mega-net.
 *
 * net_send takes what fits in the stack's 3 KB send buffer and returns
 * the count; a transfer that ignored it lost everything past the first
 * 3 KB. This loops, polling between attempts so the stack can drain, and
 * gives up when the link dies or nothing has moved for a long while (a
 * count of polls, each a call into the stack: several seconds). Kept
 * apart from netutil.c so the host tests compile it against their mocks. */
#include "netutil.h"

/* done as a percentage of size, in 16-bit arithmetic: the 32-bit
 * division library was 750 bytes for this one use. Sizes are scaled so
 * the multiply by 100 fits; a D81 file is under 1.3 MB. */
unsigned char transfer_percent(unsigned long done, unsigned long size)
{
  unsigned char sh = (size < 640UL) ? 0 : (size < 38400UL) ? 6 : 11;
  unsigned int d, s, p;
  if (size == 0) return 0;
  if (done >= size) return 100;
  d = (unsigned int)(done >> sh);
  s = (unsigned int)(size >> sh);
  p = (unsigned int)((d * 100u) / s);
  return (unsigned char)(p > 100 ? 100 : p);
}

unsigned char net_send_all(const unsigned char *p, unsigned int n)
{
  unsigned int idle = 0;
  while (n) {
    unsigned int k = net_send(p, n);
    if (k) {
      p += k;
      n -= k;
      idle = 0;
      continue;
    }
    if (!net_alive()) return 0;
    net_poll();
    if (++idle == 0) return 0;
  }
  return 1;
}
