/* Sending a whole buffer through mega-net.
 *
 * net_send takes what fits in the stack's 3 KB send buffer and returns
 * the count; a transfer that ignored it lost everything past the first
 * 3 KB. This loops, polling between attempts so the stack can drain, and
 * gives up when the link dies or nothing has moved for a long while (a
 * count of polls, each a call into the stack: several seconds). Kept
 * apart from netutil.c so the host tests compile it against their mocks. */
#include "netutil.h"

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
