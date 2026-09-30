/* ssr_net.c -- the LAN of the game's LOCAL multiplayer, in memory: two
 * copies of the engine in one process (split screen, ssr_split.c) find and
 * play each other as two phones on a Wi-Fi network did
 * (../source/docs/MULTIPLAYER.md 1).
 *
 * The game's LOCAL play is plain UDP on port 27800: the client broadcasts
 * "BRDC" to the subnet every second, the host (its lobby open) answers
 * "RPLY", the client sends "CNCT", then heartbeats and the game's packets.
 * Peers are told apart by their IP address only, the port is always 27800,
 * and the only socket calls are socket, bind, fcntl(O_NONBLOCK), setsockopt
 * (SO_BROADCAST), sendto, recvfrom (looped until it returns -1) and close,
 * all on the engine's own thread, once a logic tick.
 *
 * Here each copy of the engine has a fake address -- copy 0 (player 1)
 * 10.0.0.1, copy 1 (player 2) 10.0.0.2, the broadcast 10.0.0.255 -- which
 * javaGetMyIPAddress / javaGetBroadcastIP give it (ssr_java.c), and each UDP
 * socket a queue: sendto() puts a copy of the datagram in the queue of every
 * other copy's socket bound to the port (a broadcast) or of the one the
 * address names, with the sender's address as its source; recvfrom() takes
 * the next one (none: EAGAIN, -1, as a non-blocking socket). No loss, no
 * delay: the protocol copes with that. Sockets of other kinds keep the
 * offline model (bionic_net.c). MIT.
 */
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "bionic.h"
#include "dcr_net.h"
#include "ssr.h"
#include "util.h"

int ssr_engine_current(void); /* ssr_loader.c */

#define NET_FD_BASE 0x6000
#define NET_MAX 8
#define QCAP 256
#define DGRAM_MAX 2048

typedef struct {
  uint8_t from[4]; /* the sender's address (network order) */
  uint16_t port_be;
  uint16_t len;
  uint8_t *data;
} Dgram;

typedef struct {
  int used, eng, bound, nonblock;
  uint16_t port_be;
  Dgram q[QCAP];
  unsigned head, tail; /* head: next to read; tail: next to write */
} Sock;

static Sock g_s[NET_MAX];
static Mutex g_lock;
static unsigned long g_sent, g_dropped, g_bcast;

/* the fake address of engine copy e (network order bytes) */
static void ip_of(int e, uint8_t out[4]) {
  out[0] = 10, out[1] = 0, out[2] = 0, out[3] = (uint8_t)(1 + (e & 7));
}

/* javaGetMyIPAddress's int: the address in network order read as a
 * little-endian word (10.0.0.1 -> 0x0100000a) */
int ssr_net_my_ip(void) { return (int)(0x0100000au + ((uint32_t)ssr_engine_current() << 24)); }
int ssr_net_broadcast_ip(void) { return (int)0xff00000au; }

static Sock *sock_of(int fd) {
  const int i = fd - NET_FD_BASE;
  return i >= 0 && i < NET_MAX && g_s[i].used ? &g_s[i] : NULL;
}

int dcr_net_owns(int fd) { return sock_of(fd) != NULL; }

/* bionic_net.c's socket(): a UDP/IPv4 socket is ours; -1 lets it make its
 * offline one */
int ssr_net_socket(int domain, int type, int proto) {
  if (domain != 2 /* AF_INET */ || (type & 0xf) != 2 /* SOCK_DGRAM */)
    return -1;
  mutexLock(&g_lock);
  int fd = -1;
  for (int i = 0; i < NET_MAX && fd < 0; i++)
    if (!g_s[i].used) {
      memset(&g_s[i], 0, sizeof g_s[i]);
      g_s[i].used = 1;
      g_s[i].eng = ssr_engine_current();
      fd = NET_FD_BASE + i;
    }
  mutexUnlock(&g_lock);
  if (fd >= 0)
    debugPrintf("[net] engine %d: UDP socket %#x\n", ssr_engine_current() + 1, fd);
  return fd >= 0 ? fd : -2; /* -2: ours, but full */
}

int ssr_net_bind(int fd, const void *addr, unsigned len) {
  Sock *s = sock_of(fd);
  if (!s)
    return -1;
  if (addr && len >= 4) {
    memcpy(&s->port_be, (const uint8_t *)addr + 2, 2);
    s->bound = 1;
  }
  return 0;
}

static void push(Sock *s, const uint8_t from[4], uint16_t port_be, const void *b, size_t n) {
  const unsigned used = s->tail - s->head;
  if (used >= QCAP || n > DGRAM_MAX) {
    g_dropped++;
    return;
  }
  Dgram *d = &s->q[s->tail % QCAP];
  uint8_t *copy = malloc(n ? n : 1);
  if (!copy) {
    g_dropped++;
    return;
  }
  memcpy(copy, b, n);
  memcpy(d->from, from, 4);
  d->port_be = port_be;
  d->len = (uint16_t)n;
  d->data = copy;
  s->tail++;
}

ssize_t ssr_net_sendto(int fd, const void *b, size_t n, int flags, const void *addr, unsigned alen) {
  Sock *s = sock_of(fd);
  if (!s || !addr || alen < 8) {
    b_set_errno(L_ENETUNREACH);
    return -1;
  }
  const uint8_t *dst = (const uint8_t *)addr + 4;
  uint16_t port_be;
  memcpy(&port_be, (const uint8_t *)addr + 2, 2);
  const int bcast = dst[3] == 255;
  uint8_t me[4];
  ip_of(s->eng, me);
  mutexLock(&g_lock);
  for (int i = 0; i < NET_MAX; i++) {
    Sock *t = &g_s[i];
    if (!t->used || t == s || t->eng == s->eng || !t->bound || t->port_be != port_be)
      continue;
    uint8_t ip[4];
    ip_of(t->eng, ip);
    if (bcast || !memcmp(dst, ip, 4))
      push(t, me, s->port_be ? s->port_be : port_be, b, n);
  }
  g_sent++;
  g_bcast += bcast;
  mutexUnlock(&g_lock);
  return (ssize_t)n;
}

ssize_t ssr_net_recvfrom(int fd, void *b, size_t n, int flags, void *addr, unsigned *alen) {
  Sock *s = sock_of(fd);
  if (!s) {
    b_set_errno(L_EBADF);
    return -1;
  }
  mutexLock(&g_lock);
  if (s->head == s->tail) {
    mutexUnlock(&g_lock);
    b_set_errno(L_EAGAIN);
    return -1;
  }
  Dgram *d = &s->q[s->head % QCAP];
  const size_t k = d->len < n ? d->len : n;
  memcpy(b, d->data, k);
  if (addr && alen && *alen >= 8) {
    uint8_t *a = addr;
    memset(a, 0, *alen < 16 ? *alen : 16);
    a[0] = 2, a[1] = 0; /* sin_family AF_INET (little-endian u16) */
    memcpy(a + 2, &d->port_be, 2);
    memcpy(a + 4, d->from, 4);
    *alen = 16;
  }
  free(d->data);
  d->data = NULL;
  s->head++;
  mutexUnlock(&g_lock);
  return (ssize_t)k;
}

int dcr_net_close(int fd) {
  Sock *s = sock_of(fd);
  if (!s)
    return -1;
  mutexLock(&g_lock);
  while (s->head != s->tail) {
    free(s->q[s->head % QCAP].data);
    s->head++;
  }
  s->used = 0;
  mutexUnlock(&g_lock);
  debugPrintf("[net] socket %#x closed (%lu datagrams sent so far, %lu broadcasts, %lu dropped)\n", fd, g_sent,
              g_bcast, g_dropped);
  return 0;
}

int dcr_net_fcntl(int fd, int cmd, long arg) {
  Sock *s = sock_of(fd);
  if (!s)
    return -1;
  if (cmd == L_F_SETFL)
    s->nonblock = (arg & L_O_NONBLOCK) != 0;
  if (cmd == L_F_GETFL)
    return L_O_RDWR | (s->nonblock ? L_O_NONBLOCK : 0);
  return 0;
}

int dcr_net_ioctl(int fd, unsigned long req, void *arg) { return 0; }

short dcr_net_ready(int fd, short events) {
  Sock *s = sock_of(fd);
  if (!s)
    return 0;
  short r = events & 4; /* POLLOUT: always */
  if ((events & 1) && s->head != s->tail)
    r |= 1; /* POLLIN */
  return r;
}
