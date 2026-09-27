/* mega-net from the client's side: bringing it up, names, one TCP
 * connection. Every wait is bounded on $D7FA. */
#ifndef NETUTIL_H
#define NETUTIL_H

#include <stdint.h>
#include "meganet.h"

extern unsigned int net_frames;      /* frames counted by net_poll(), for callers' timeouts */
extern unsigned char net_ready;      /* mega-net loaded and INIT run */

void net_poll(void);                 /* mega-net's POLL, and the frame count */

/* Quiets the controller, loads MEGANET from the boot disk, runs INIT. */
unsigned char net_load(const char **err);
/* A DHCP lease, unless one is held. */
unsigned char net_dhcp(const char **err);
/* Dotted quad or DNS. */
unsigned char net_resolve(const char *host, unsigned char *ip, const char **err);

/* Socket 0: connect within a bound; returns 0 with a message. */
unsigned char net_connect(const unsigned char *ip, unsigned int port, const char **err);
/* Sends what it can of n bytes; returns how many were taken. */
unsigned int net_send(const unsigned char *p, unsigned int n);
/* Sends all n bytes, polling while the stack drains (netsend.c); 0 if the
 * link died or nothing moved for 1500 frames. */
unsigned char net_send_all(const unsigned char *p, unsigned int n);
/* Receives up to cap bytes; 0 if none are waiting. */
unsigned int net_recv(unsigned char *buf, unsigned int cap);
/* 1 while the peer may still send: not closed, or closed with data still queued. */
unsigned char net_alive(void);
void net_close(void);
void net_abort(void);

#endif
