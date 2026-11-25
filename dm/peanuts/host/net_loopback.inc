// Included by hle_net.c so readiness can share the eventfd/co-op poll path.
#include "net_loopback.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <strings.h>
#include <unistd.h>

enum { LO_BASE = 576, LO_MAX = 64, LO_AF_INET = 2 };
_Static_assert(COOP_FD_BASE + DM_COOP_FD_MAX <= LO_BASE, "co-op and loopback ranges overlap");

typedef struct {
    uint8_t len, family;
    uint16_t port;
    uint32_t ip;
    uint8_t padding[8];
} lo_addr;
_Static_assert(sizeof(lo_addr) == 16, "Horizon sockaddr_in");

typedef struct {
    uint64_t generation;
    int fd, type, flags, cloexec, bound;
    int64_t recv_timeout, send_timeout;
} lo_socket;

static lo_socket lo_sockets[LO_MAX];
static pthread_mutex_t lo_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int lo_refused, lo_logged[3];

int dm_lo_enabled(void) {
    const char *value = getenv("DM_NET_LOOPBACK");
    if (value && !strcmp(value, "0")) return 0;
    if (value && !strcmp(value, "1")) return 1;
    return DM_GAME_NET_LOOPBACK;
}

static int lo_error(int e) { dm_sk_seterr(e); return -1; }

static int lo_errno(int e) {
    switch (e) {
#define LO_ERR(host, guest) case host: return guest
    LO_ERR(EPERM, 1); LO_ERR(ENOENT, 2); LO_ERR(EINTR, 4); LO_ERR(EIO, 5);
    LO_ERR(EBADF, 9); LO_ERR(EACCES, 13); LO_ERR(EFAULT, 14); LO_ERR(EINVAL, 22);
    LO_ERR(ENFILE, 23); LO_ERR(EMFILE, 24); LO_ERR(EPIPE, 32);
    LO_ERR(EAGAIN, 35); LO_ERR(EINPROGRESS, 36); LO_ERR(EALREADY, 37);
    LO_ERR(ENOTSOCK, 38); LO_ERR(EDESTADDRREQ, 39); LO_ERR(EMSGSIZE, 40);
    LO_ERR(EPROTOTYPE, 41); LO_ERR(ENOPROTOOPT, 42); LO_ERR(EPROTONOSUPPORT, 43);
    LO_ERR(EOPNOTSUPP, 45); LO_ERR(EAFNOSUPPORT, 47); LO_ERR(EADDRINUSE, 48);
    LO_ERR(EADDRNOTAVAIL, 49); LO_ERR(ENETDOWN, 50); LO_ERR(ENETUNREACH, 51);
    LO_ERR(ENETRESET, 52); LO_ERR(ECONNABORTED, 53); LO_ERR(ECONNRESET, 54);
    LO_ERR(ENOBUFS, 55); LO_ERR(EISCONN, 56); LO_ERR(ENOTCONN, 57);
    LO_ERR(ESHUTDOWN, 58); LO_ERR(ETIMEDOUT, 60); LO_ERR(ECONNREFUSED, 61);
    LO_ERR(EHOSTUNREACH, 65); LO_ERR(ENOMEM, 12);
#undef LO_ERR
    default: return 5; // EIO
    }
}

static int64_t lo_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static lo_socket *lo_find(int guest) { // caller holds lo_mu
    if (guest < LO_BASE || guest >= LO_BASE + LO_MAX) return NULL;
    lo_socket *s = &lo_sockets[guest - LO_BASE];
    return s->generation & 1 ? s : NULL;
}

static int lo_exists(int guest) {
    pthread_mutex_lock(&lo_mu);
    int exists = lo_find(guest) != NULL;
    pthread_mutex_unlock(&lo_mu);
    return exists;
}

// All host descriptors are nonblocking. Holding the lock through each syscall
// prevents a concurrent close from sending that syscall to a recycled host fd.
static lo_socket *lo_lock(int guest) {
    pthread_mutex_lock(&lo_mu);
    lo_socket *s = lo_find(guest);
    if (!s) {
        pthread_mutex_unlock(&lo_mu);
        lo_error(SK_EBADF);
    }
    return s;
}

static long lo_finish(long ret) {
    int e = errno;
    pthread_mutex_unlock(&lo_mu);
    return ret < 0 ? lo_error(lo_errno(e)) : ret;
}

static int lo_wait(lo_socket *s, int64_t deadline) { // lock released only during the cooperative sleep
    uint64_t generation = s->generation;
    int64_t ns = deadline ? deadline - lo_now() : 2000000;
    if (ns <= 0) { errno = EAGAIN; return 0; }
    pthread_mutex_unlock(&lo_mu);
    dm_guest_sleep_ns(ns < 2000000 ? ns : 2000000);
    pthread_mutex_lock(&lo_mu);
    if (s->generation == generation) return 1;
    errno = EBADF;
    return 0;
}

static int lo_adopt(int fd, int type, int flags, int cloexec, int bound) {
    if (fd < 0) return -1;
    int i;
    for (i = 0; i < LO_MAX && (lo_sockets[i].generation & 1); i++) {}
    int e = EMFILE;
    if (i < LO_MAX) {
#ifdef SO_NOSIGPIPE
        int one = 1;
        if (setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) < 0) goto fail;
#endif
        if (fcntl(fd, F_SETFL, O_NONBLOCK) < 0 || fcntl(fd, F_SETFD, FD_CLOEXEC) < 0) goto fail;
        lo_sockets[i] = (lo_socket){.generation = lo_sockets[i].generation + 1, .fd = fd, .type = type,
                                   .flags = flags, .cloexec = cloexec, .bound = bound};
        return LO_BASE + i;
    }
    goto close_fd;
fail:
    e = errno;
close_fd:
    close(fd);
    errno = e;
    return -1;
}

int dm_lo_socket(int family, int type, int protocol) {
    const int nonblock = 0x20000000, cloexec = 0x10000000;
    int kind = type & ~(nonblock | cloexec);
    if (family != LO_AF_INET) return lo_error(47);
    if (kind != 1 && kind != 2) return lo_error(44);
    if (protocol && protocol != (kind == 1 ? IPPROTO_TCP : IPPROTO_UDP)) return lo_error(43);
    pthread_mutex_lock(&lo_mu);
    int fd = socket(AF_INET, kind == 1 ? SOCK_STREAM : SOCK_DGRAM, protocol);
    return (int)lo_finish(lo_adopt(fd, kind, type & nonblock ? SK_O_NONBLOCK : 0, !!(type & cloexec), 0));
}

static struct sockaddr_in lo_endpoint(uint16_t port, uint32_t ip) {
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = port, .sin_addr.s_addr = ip};
#ifdef __APPLE__
    addr.sin_len = sizeof addr;
#endif
    return addr;
}

static int lo_is_local(uint32_t ip) { return (ntohl(ip) >> 24) == 127; }

static int lo_input(const void *guest, unsigned len, struct sockaddr_in *addr, int binding) {
    lo_addr a;
    if (!guest || len < sizeof a) return lo_error(SK_EINVAL);
    memcpy(&a, guest, sizeof a);
    if (a.family != LO_AF_INET) return lo_error(47);
    if (!binding && !lo_is_local(a.ip)) {
        if (!atomic_exchange(&lo_refused, 1))
            LOG("[RED] net: refused non-loopback destination %#x:%u", (unsigned)ntohl(a.ip), (unsigned)ntohs(a.port));
        return lo_error(51);
    }
    *addr = lo_endpoint(a.port, binding ? htonl(INADDR_LOOPBACK) : a.ip);
    return 0;
}

static void lo_output(void *guest, unsigned *len, const struct sockaddr_in *addr) {
    if (!guest || !len) return;
    lo_addr a = {.len = sizeof a, .family = LO_AF_INET, .port = addr->sin_port, .ip = addr->sin_addr.s_addr};
    memcpy(guest, &a, *len < sizeof a ? *len : sizeof a);
    *len = sizeof a;
}

static int lo_bind(lo_socket *s, const struct sockaddr_in *addr) {
    int rc = bind(s->fd, (const struct sockaddr *)addr, sizeof *addr);
    if (rc == 0) {
        s->bound = 1;
        if (!atomic_exchange(&lo_logged[s->type], 1)) {
            struct sockaddr_in actual = *addr;
            socklen_t len = sizeof actual;
            getsockname(s->fd, (struct sockaddr *)&actual, &len);
            LOG("net: loopback socket %s bound 127.0.0.1:%u", s->type == 1 ? "TCP" : "UDP", (unsigned)ntohs(actual.sin_port));
        }
    }
    return rc;
}

static int lo_ensure_bound(lo_socket *s) {
    struct sockaddr_in addr = lo_endpoint(0, htonl(INADDR_LOOPBACK));
    return s->bound ? 0 : lo_bind(s, &addr);
}

int dm_lo_bind(int fd, const void *guest, unsigned len) {
    struct sockaddr_in addr;
    if (lo_input(guest, len, &addr, 1) < 0) return -1;
    lo_socket *s = lo_lock(fd);
    return s ? (int)lo_finish(lo_bind(s, &addr)) : -1;
}

int dm_lo_connect(int fd, const void *guest, unsigned len) {
    struct sockaddr_in addr;
    if (lo_input(guest, len, &addr, 0) < 0) return -1;
    lo_socket *s = lo_lock(fd);
    if (!s) return -1;
    if (lo_ensure_bound(s) < 0) return (int)lo_finish(-1);
    int rc = connect(s->fd, (struct sockaddr *)&addr, sizeof addr);
    if (rc == 0 || errno != EINPROGRESS || s->flags) return (int)lo_finish(rc);
    int64_t deadline = s->send_timeout ? lo_now() + s->send_timeout : 0;
    struct pollfd p = {s->fd, POLLOUT, 0};
    while ((rc = poll(&p, 1, 0)) == 0) {
        if (!lo_wait(s, deadline)) {
            if (errno == EAGAIN) errno = EINPROGRESS;
            return (int)lo_finish(-1);
        }
    }
    if (rc < 0) return (int)lo_finish(-1);
    int error = 0;
    socklen_t size = sizeof error;
    if (getsockopt(s->fd, SOL_SOCKET, SO_ERROR, &error, &size) < 0) return (int)lo_finish(-1);
    errno = error;
    return (int)lo_finish(error ? -1 : 0);
}

int dm_lo_listen(int fd, int backlog) {
    lo_socket *s = lo_lock(fd);
    if (!s) return -1;
    return (int)lo_finish(lo_ensure_bound(s) < 0 ? -1 : listen(s->fd, backlog));
}

int dm_lo_accept(int fd, void *guest, unsigned *len) {
    if (guest && !len) return lo_error(SK_EINVAL);
    lo_socket *s = lo_lock(fd);
    if (!s) return -1;
    int64_t deadline = s->recv_timeout ? lo_now() + s->recv_timeout : 0;
    struct sockaddr_in addr;
    socklen_t size;
    int accepted;
    do {
        size = sizeof addr;
        accepted = accept(s->fd, (struct sockaddr *)&addr, &size);
    } while (accepted < 0 && errno == EAGAIN && !s->flags && lo_wait(s, deadline));
    if (accepted >= 0 && !lo_is_local(addr.sin_addr.s_addr)) {
        close(accepted);
        errno = ENETUNREACH;
        accepted = -1;
    }
    int result = lo_adopt(accepted, s->type, s->flags, 0, 1);
    if (result >= 0) lo_output(guest, len, &addr);
    return (int)lo_finish(result);
}

int dm_lo_close(int fd) {
    lo_socket *s = lo_lock(fd);
    if (!s) return -1;
    s->generation++;
    return (int)lo_finish(close(s->fd));
}

int dm_lo_name(int fd, void *guest, unsigned *len, int peer) {
    if (!guest || !len) return lo_error(SK_EINVAL);
    lo_socket *s = lo_lock(fd);
    if (!s) return -1;
    struct sockaddr_in addr;
    socklen_t size = sizeof addr;
    int rc = peer ? getpeername(s->fd, (struct sockaddr *)&addr, &size) :
                    getsockname(s->fd, (struct sockaddr *)&addr, &size);
    if (!rc) lo_output(guest, len, &addr);
    return (int)lo_finish(rc);
}

int dm_lo_fcntl(int fd, int cmd, long arg) {
    lo_socket *s = lo_lock(fd);
    if (!s) return -1;
    int rc = 0;
    switch (cmd) {
    case SK_F_GETFL: rc = s->flags | 2; break; // O_RDWR
    case SK_F_SETFL: s->flags = arg & SK_O_NONBLOCK; break;
    case SK_F_GETFD: rc = s->cloexec; break;
    case SK_F_SETFD: s->cloexec = arg & 1; break;
    default: errno = EINVAL; rc = -1; break;
    }
    return (int)lo_finish(rc);
}

int dm_lo_shutdown(int fd, int how) {
    const int methods[] = {SHUT_RD, SHUT_WR, SHUT_RDWR};
    if (how < 0 || how > 2) return lo_error(SK_EINVAL);
    lo_socket *s = lo_lock(fd);
    return s ? (int)lo_finish(shutdown(s->fd, methods[how])) : -1;
}

static int lo_flags(int flags, int sending) {
    const struct { int guest, host; } map[] = {
        {1, MSG_OOB}, {2, MSG_PEEK}, {4, MSG_DONTROUTE}, {8, MSG_EOR},
        {0x10, MSG_TRUNC}, {0x40, MSG_WAITALL}, {0x80, MSG_DONTWAIT},
        {0x20000, 0},
    };
    int host = 0;
#ifdef MSG_NOSIGNAL
    if (sending) host |= MSG_NOSIGNAL;
#else
    (void)sending;
#endif
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) {
        if (flags & map[i].guest) host |= map[i].host;
        flags &= ~map[i].guest;
    }
    return flags ? lo_error(45) : host;
}

long dm_lo_send(int fd, const void *buf, unsigned long size, int flags, const void *guest, unsigned len) {
    struct sockaddr_in addr;
    if (guest && lo_input(guest, len, &addr, 0) < 0) return -1;
    int host_flags = lo_flags(flags, 1);
    if (host_flags < 0) return -1;
    lo_socket *s = lo_lock(fd);
    if (!s) return -1;
    if (lo_ensure_bound(s) < 0) return lo_finish(-1);
    int64_t deadline = s->send_timeout ? lo_now() + s->send_timeout : 0;
    long rc;
    do {
        rc = sendto(s->fd, buf, size, host_flags, guest ? (struct sockaddr *)&addr : NULL, guest ? sizeof addr : 0);
    } while (rc < 0 && errno == EAGAIN && !s->flags && !(host_flags & MSG_DONTWAIT) && lo_wait(s, deadline));
    return lo_finish(rc);
}

long dm_lo_recv(int fd, void *buf, unsigned long size, int flags, void *guest, unsigned *len) {
    if (guest && !len) return lo_error(SK_EINVAL);
    int host_flags = lo_flags(flags, 0);
    if (host_flags < 0) return -1;
    lo_socket *s = lo_lock(fd);
    if (!s) return -1;
    int64_t deadline = s->recv_timeout ? lo_now() + s->recv_timeout : 0;
    struct sockaddr_in addr;
    socklen_t addr_size;
    long rc;
    unsigned long total = 0;
    for (;;) {
        memset(&addr, 0, sizeof addr);
        addr_size = sizeof addr;
        rc = recvfrom(s->fd, total ? (char *)buf + total : buf, size - total, host_flags,
                      guest ? (struct sockaddr *)&addr : NULL, guest ? &addr_size : NULL);
        if (rc < 0 && errno == EAGAIN && !s->flags && !(host_flags & MSG_DONTWAIT) && lo_wait(s, deadline)) continue;
        if (rc <= 0) break;
        total += (unsigned long)rc;
        if (total >= size || s->type != 1 || !(host_flags & MSG_WAITALL) ||
            (host_flags & (MSG_PEEK | MSG_DONTWAIT | MSG_OOB)) || s->flags) break;
    }
    if (rc >= 0 && addr_size && guest && addr.sin_family == AF_INET) lo_output(guest, len, &addr);
    else if (rc >= 0 && guest) *len = 0;
    return lo_finish(total ? (long)total : rc);
}

typedef struct { int64_t sec, usec; } lo_timeval;

static int lo_option(int *level, int opt) {
    if (*level == 6) {
        *level = IPPROTO_TCP;
        return opt == 1 ? TCP_NODELAY : -1;
    }
    if (*level != 0xffff) return -1;
    *level = SOL_SOCKET;
    switch (opt) {
    case 0x0004: return SO_REUSEADDR;
    case 0x0008: return SO_KEEPALIVE;
    case 0x0010: return SO_DONTROUTE;
    case 0x0020: return SO_BROADCAST;
    case 0x0080: return SO_LINGER;
    case 0x0100: return SO_OOBINLINE;
    case 0x0200: return SO_REUSEPORT;
    case 0x1001: return SO_SNDBUF;
    case 0x1002: return SO_RCVBUF;
    case 0x1003: return SO_SNDLOWAT;
    case 0x1004: return SO_RCVLOWAT;
    case 0x1007: return SO_ERROR;
    case 0x1008: return SO_TYPE;
    case 0x0002: return SO_ACCEPTCONN;
    default: return -1;
    }
}

static int64_t *lo_timeout(lo_socket *s, int level, int opt) {
    if (level != 0xffff) return NULL;
    if (opt == 0x1005) return &s->send_timeout;
    if (opt == 0x1006) return &s->recv_timeout;
    return NULL;
}

int dm_lo_getopt(int fd, int level, int opt, void *value, unsigned *len) {
    if (!value || !len) return lo_error(SK_EINVAL);
    lo_socket *s = lo_lock(fd);
    if (!s) return -1;
    int64_t *timeout = lo_timeout(s, level, opt);
    if (timeout) {
        lo_timeval tv = {*timeout / 1000000000, (*timeout % 1000000000) / 1000};
        *len = *len < sizeof tv ? *len : sizeof tv;
        memcpy(value, &tv, *len);
        return (int)lo_finish(0);
    }
    int host_opt = lo_option(&level, opt);
    if (host_opt < 0) { errno = ENOPROTOOPT; return (int)lo_finish(-1); }
    socklen_t size = *len;
    if (level == SOL_SOCKET && (host_opt == SO_ERROR || host_opt == SO_TYPE)) {
        int out;
        size = sizeof out;
        if (getsockopt(s->fd, level, host_opt, &out, &size) < 0) return (int)lo_finish(-1);
        out = host_opt == SO_TYPE ? s->type : out ? lo_errno(out) : 0;
        *len = *len < sizeof out ? *len : sizeof out;
        memcpy(value, &out, *len);
        return (int)lo_finish(0);
    }
    int rc = getsockopt(s->fd, level, host_opt, value, &size);
    if (!rc) *len = size;
    return (int)lo_finish(rc);
}

int dm_lo_setopt(int fd, int level, int opt, const void *value, unsigned len) {
    if (!value) return lo_error(SK_EINVAL);
    lo_socket *s = lo_lock(fd);
    if (!s) return -1;
    int64_t *timeout = lo_timeout(s, level, opt);
    if (timeout) {
        lo_timeval tv;
        if (len < sizeof tv) { errno = EINVAL; return (int)lo_finish(-1); }
        memcpy(&tv, value, sizeof tv);
        if (tv.sec < 0 || tv.sec > INT_MAX || tv.usec < 0 || tv.usec >= 1000000) {
            errno = EINVAL;
            return (int)lo_finish(-1);
        }
        *timeout = tv.sec * 1000000000 + tv.usec * 1000;
        return (int)lo_finish(0);
    }
    int host_opt = lo_option(&level, opt);
    if (host_opt < 0) { errno = ENOPROTOOPT; return (int)lo_finish(-1); }
    // A positive linger could make close wait in the kernel while holding lo_mu.
    if (level == SOL_SOCKET && host_opt == SO_LINGER && len >= sizeof(struct linger)) {
        struct linger linger;
        memcpy(&linger, value, sizeof linger);
        if (linger.l_onoff && linger.l_linger != 0) { errno = EOPNOTSUPP; return (int)lo_finish(-1); }
    }
    return (int)lo_finish(setsockopt(s->fd, level, host_opt, value, len));
}

enum { LO_POLLPRI = 0x02, LO_POLLERR = 0x08, LO_POLLHUP = 0x10, LO_POLLRDBAND = 0x80, LO_POLLWRBAND = 0x100 };

static int lo_ready(int fd, int want, int selecting) {
    if (fd < 0) return 0;
    pthread_mutex_lock(&lo_mu);
    lo_socket *s = lo_find(fd);
    if (!s) {
        pthread_mutex_unlock(&lo_mu);
        return SK_POLLNVAL;
    }
    const struct { short guest, host; } bits[] = {
        {SK_POLLIN, POLLIN}, {SK_POLLRDNORM, POLLRDNORM}, {LO_POLLPRI, POLLPRI},
        {LO_POLLRDBAND, POLLRDBAND}, {SK_POLLOUT, POLLOUT}, {LO_POLLWRBAND, POLLWRBAND},
    };
    struct pollfd p = {.fd = s->fd};
    for (size_t i = 0; i < sizeof bits / sizeof *bits; i++)
        if (want & bits[i].guest) p.events |= bits[i].host;
    int rc = poll(&p, 1, 0);
    pthread_mutex_unlock(&lo_mu);
    if (rc < 0) return LO_POLLERR;
    if (selecting) {
        if (p.revents & POLLNVAL) return SK_POLLNVAL;
        if ((want & SK_POLLIN) && (p.revents & (POLLIN | POLLHUP | POLLERR))) return SK_POLLIN;
        if ((want & SK_POLLOUT) && (p.revents & (POLLOUT | POLLERR))) return SK_POLLOUT;
        if ((want & LO_POLLPRI) && (p.revents & POLLPRI)) return LO_POLLPRI;
        return 0;
    }
    int ready = 0;
    for (size_t i = 0; i < sizeof bits / sizeof *bits; i++)
        if ((want & bits[i].guest) && (p.revents & bits[i].host)) ready |= bits[i].guest;
    if (p.revents & POLLERR) ready |= LO_POLLERR;
    if (p.revents & POLLHUP) ready |= LO_POLLHUP;
    if (p.revents & POLLNVAL) ready |= SK_POLLNVAL;
    return ready;
}

static int lo_lookup(const char *name, uint32_t *ip) {
    if (!name) return 0;
    if (!strcasecmp(name, "localhost")) { *ip = htonl(INADDR_LOOPBACK); return 1; }
    struct in_addr addr;
    if (inet_pton(AF_INET, name, &addr) != 1) return 0;
    *ip = addr.s_addr;
    return 1;
}

void *dm_lo_host(const char *name) {
    static __thread struct {
        char *name, **aliases;
        int family, length;
        char **addresses;
    } ent;
    static __thread char label[16], *aliases[1], *addresses[2];
    static __thread uint32_t address;
    if (!lo_lookup(name, &address)) { sk_fail(); return NULL; }
    snprintf(label, sizeof label, "%s", name);
    addresses[0] = (char *)&address;
    ent.name = label;
    ent.aliases = aliases;
    ent.family = LO_AF_INET;
    ent.length = sizeof address;
    ent.addresses = addresses;
    return &ent;
}

// SDK 10's CopyFromPlatform stores the typed family as a byte and swaps
// the two pointer fields relative to plain BSD addrinfo.
typedef struct lo_ai_plain {
    int flags, family, type, protocol;
    uint32_t addrlen;
    char *canonname;
    lo_addr *addr;
    struct lo_ai_plain *next;
} lo_ai_plain;

typedef struct lo_ai_nn {
    int flags;
    uint8_t family;
    int type, protocol;
    uint32_t addrlen;
    lo_addr *addr;
    char *canonname;
    struct lo_ai_nn *next;
} lo_ai_nn;

_Static_assert(sizeof(lo_ai_plain) == 48 && offsetof(lo_ai_plain, addr) == 32, "BSD addrinfo layout");
_Static_assert(sizeof(lo_ai_nn) == 48 && offsetof(lo_ai_nn, addr) == 24, "SDK AddrInfo layout");

typedef struct {
    union {
        lo_ai_plain plain[2];
        lo_ai_nn nn[2];
    } info;
    lo_addr addr;
    char name[16];
} lo_ai_result;

int dm_lo_gai(const char *host, const char *service, const void *hint, void **result, int nn_abi) {
    if (!result) return 4;
    *result = NULL;
    if (!dm_lo_enabled()) return 4;
    uint32_t ip;
    if (!lo_lookup(host, &ip)) return 4;
    unsigned long port = 0;
    if (service) {
        char *end;
        if (!*service || service[strspn(service, "0123456789")]) return 9; // EAI_SERVICE
        port = strtoul(service, &end, 10);
        if (*end || port > UINT16_MAX) return 9;
    }
    lo_ai_plain h = {0};
    if (hint && nn_abi) {
        lo_ai_nn typed;
        memcpy(&typed, hint, sizeof typed);
        h.flags = typed.flags;
        h.family = typed.family;
        h.type = typed.type;
        h.protocol = typed.protocol;
    } else if (hint) {
        memcpy(&h, hint, sizeof h);
    }
    if (h.family && h.family != LO_AF_INET) return 5; // EAI_FAMILY
    if (h.type && h.type != 1 && h.type != 2) return 10; // EAI_SOCKTYPE
    if (h.protocol && h.protocol != IPPROTO_TCP && h.protocol != IPPROTO_UDP) return 9;
    int type = h.type ? h.type : h.protocol == IPPROTO_TCP ? 1 : h.protocol == IPPROTO_UDP ? 2 : 0;
    if (type && h.protocol && (type == 1 ? IPPROTO_TCP : IPPROTO_UDP) != h.protocol) return 9;
    if ((h.flags & 4) && !strcasecmp(host, "localhost")) return 8; // AI_NUMERICHOST
    lo_ai_result *a = calloc(1, sizeof *a);
    if (!a) return 6; // EAI_MEMORY
    a->addr = (lo_addr){.len = sizeof(lo_addr), .family = LO_AF_INET, .port = htons((uint16_t)port), .ip = ip};
    snprintf(a->name, sizeof a->name, "%s", host);
    int count = type ? 1 : 2;
    for (int i = 0; i < count; i++) {
        int kind = type ? type : i + 1;
        int protocol = kind == 1 ? IPPROTO_TCP : IPPROTO_UDP;
        char *name = i == 0 && (h.flags & 2) ? a->name : NULL; // AI_CANONNAME
        if (nn_abi) {
            a->info.nn[i] = (lo_ai_nn){.flags = h.flags, .family = LO_AF_INET, .type = kind,
                                      .protocol = protocol, .addrlen = sizeof a->addr,
                                      .addr = &a->addr, .canonname = name,
                                      .next = i + 1 < count ? &a->info.nn[i + 1] : NULL};
        } else {
            a->info.plain[i] = (lo_ai_plain){.flags = h.flags, .family = LO_AF_INET, .type = kind,
                                            .protocol = protocol, .addrlen = sizeof a->addr,
                                            .addr = &a->addr, .canonname = name,
                                            .next = i + 1 < count ? &a->info.plain[i + 1] : NULL};
        }
    }
    *result = a;
    return 0;
}
