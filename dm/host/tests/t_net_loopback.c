// PEANUTS(upstream-candidate): exercise the Horizon socket hooks with real local traffic.
#include "../hle.h"
#pragma push_macro("HLE_FORCE")
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret plain_##cname(__VA_ARGS__)
#include "../hle_offline.c"
#pragma pop_macro("HLE_FORCE")
#include "../hle_net.c"

void *guest_sym(const char *name) { (void)name; return NULL; }
void dm_guest_sleep_ns(int64_t ns) {
    struct timespec ts = {ns / 1000000000, ns % 1000000000};
    nanosleep(&ts, NULL);
}

static int checks;
#define CHECK(c) do { checks++; if (!(c)) { fprintf(stderr, "FAIL line %d (%s, BSD errno %d)\n", __LINE__, #c, t_skerr); exit(1); } } while (0)

static void setbit(uint64_t *set, int fd) { set[fd / 64] |= 1ull << (fd % 64); }
static int hasbit(const uint64_t *set, int fd) { return (set[fd / 64] >> (fd % 64)) & 1; }

static lo_addr bind_local(int fd, uint32_t ip) {
    lo_addr addr = {.len = 16, .family = 2, .ip = ip};
    CHECK(plain_h_sk_Bind(fd, &addr, sizeof addr) == 0);
    unsigned n = sizeof addr;
    CHECK(h_nsk_GetSockName(fd, &addr, &n) == 0);
    CHECK(n == 16 && addr.len == 16 && addr.family == 2 && addr.port && addr.ip == htonl(0x7f000001));
    return addr;
}

typedef struct { int fd; lo_addr to; } delayed_send;
static void *send_later(void *arg) {
    delayed_send *s = arg;
    dm_guest_sleep_ns(15000000);
    return (void *)(intptr_t)h_nsk_SendTo(s->fd, "w", 1, 0, &s->to, sizeof s->to);
}

static void test_udp(void) {
    int a = plain_h_sk_Socket3i(2, 2, 17);
    int b = h_nsk_Socket(2, 2, 0);
    CHECK(a >= 576 && b > a && b < 640);
    lo_addr dest = bind_local(a, htonl(0xc0000201)); // A guest bind to any IPv4 address is confined to loopback.
    lo_addr src = bind_local(b, 0);
    int one = 1, option = 0;
    unsigned n = sizeof option;
    CHECK(plain_h_sk_SetSockOpt2(b, 0xffff, 0x20, &one, sizeof one) == 0);
    CHECK(h_nsk_GetSockOpt(b, 0xffff, 0x20, &option, &n) == 0 && option == 1);
    char byte = 0;
    CHECK(h_nsk_Fcntl(a, 4, 4) == 0 && (h_nsk_Fcntl(a, 3, 0) & 4));
    CHECK(h_nsk_Recv(a, &byte, 1, 0) == -1 && h_nsk_GetErrno() == 35);
    CHECK(h_nsk_Fcntl(a, 4, 0) == 0);
    CHECK(h_nsk_Recv(a, &byte, 1, 0x80) == -1 && plain_h_sk_Errno() == 35);

    lo_addr remote = dest;
    remote.ip = htonl(0xc0000201);
    CHECK(h_nsk_SendTo(b, "x", 1, 0, &remote, sizeof remote) == -1 && h_sk_GetLastError() == 51);
    CHECK(h_nsk_Connect(b, &remote, sizeof remote) == -1 && h_sk_GetLastError() == 51);
    remote.ip = 0;
    CHECK(h_nsk_SendTo(b, "x", 1, 0, &remote, sizeof remote) == -1 && h_sk_GetLastError() == 51);

    CHECK(plain_h_sk_SendTo(b, "u", 1, 0, &dest, sizeof dest) == 1);
    int efd = h_sk_EventFd(1, 0);
    CHECK(efd >= 512 && efd < 576);
    sk_pollfd p[] = {{a, 0x40, 0}, {efd, 1, 0}, {-1, 1, -1}, {42, 1, 0}};
    CHECK(h_nsk_Poll(p, 4, 0) == 3 && p[0].revents == 0x40 && p[1].revents == 1 &&
          p[2].revents == 0 && p[3].revents == 0x20);
    int64_t tv[2] = {0, 0};
    uint64_t r[16] = {0}, w[16] = {0}, e[16] = {0};
    setbit(r, a); setbit(r, efd); setbit(w, a); setbit(e, a);
    CHECK(plain_h_sk_Select2(1024, r, w, e, tv) == 3);
    CHECK(hasbit(r, a) && hasbit(r, efd) && hasbit(w, a) && !hasbit(e, a));
    lo_addr from = {0};
    n = sizeof from;
    CHECK(plain_h_sk_RecvFrom2(a, &byte, 1, 2, &from, &n) == 1 && byte == 'u');
    CHECK(n == 16 && from.port == src.port && from.ip == src.ip);
    CHECK(h_nsk_RecvFrom(a, &byte, 1, 0, NULL, NULL) == 1 && byte == 'u');

    lo_timeval timeout = {0, 100000};
    CHECK(h_nsk_SetSockOpt(a, 0xffff, 0x1006, &timeout, sizeof timeout) == 0);
    delayed_send send = {b, dest};
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, send_later, &send) == 0);
    CHECK(h_nsk_Recv(a, &byte, 1, 0) == 1 && byte == 'w');
    void *sent;
    CHECK(pthread_join(thread, &sent) == 0 && (intptr_t)sent == 1);
    CHECK(h_nsk_Recv(a, &byte, 1, 0) == -1 && h_nsk_GetErrno() == 35);
    CHECK(h_nsk_Close(a) == 0 && h_nsk_Close(b) == 0 && h_nsk_Close(efd) == 0);
    CHECK(dm_sk_poll(p, 1, 0) == 1 && p[0].revents == 0x20);
}

static void test_tcp(void) {
    int server = h_sk_Socket(2, 1, 6);
    CHECK(server >= 576);
    lo_addr addr = bind_local(server, 0);
    int refused = h_nsk_Socket(2, 1, 0);
    CHECK(refused >= 576);
    CHECK(h_nsk_Connect(refused, &addr, sizeof addr) == -1 && h_nsk_GetErrno() == 61);
    CHECK(h_nsk_Close(refused) == 0);
    CHECK(plain_h_c_sk_Listen(server, 2) == 0);
    int client = h_nsk_Socket(2, 1, 0);
    CHECK(client >= 576);
    CHECK(h_nsk_Bind(client, &addr, sizeof addr) == -1 && h_nsk_GetErrno() == 48);
    CHECK(h_nsk_Connect(client, &addr, sizeof addr) == 0);
    int peer = plain_h_c_sk_Accept(server, NULL, NULL);
    CHECK(peer >= 576 && peer != client && peer != server);
    lo_addr name = {0};
    unsigned n = sizeof name;
    CHECK(h_nsk_GetPeerName(client, &name, &n) == 0 && name.port == addr.port && name.ip == addr.ip);
    CHECK(h_nsk_Send(client, "TCP", 3, 0) == 3);
    char bytes[4] = {0};
    CHECK(h_sk_Recv(peer, bytes, 3, 0x40) == 3 && !strcmp(bytes, "TCP"));
    CHECK(plain_h_c_sk_Shutdown(client, 1) == 0);
    CHECK(h_sk_Recv(peer, bytes, sizeof bytes, 0) == 0);
    CHECK(h_sk_Close(peer) == 0 && h_sk_Close(client) == 0 && h_sk_Close(server) == 0);
}

static void test_lookup(void) {
    CHECK(h_sk_GetHostByName("localhost") != NULL);
    CHECK(h_nsk_GetHost("127.3.4.5", NULL, 0) != NULL);
    struct {
        char *name, **aliases;
        int family, length;
        char **addresses;
    } entry;
    const void *host = h_sk_GetHostByName("0.0.0.0");
    uint32_t ip;
    CHECK(host != NULL);
    memcpy(&entry, host, sizeof entry);
    CHECK(entry.family == 2 && entry.length == sizeof ip &&
          entry.addresses[0] && !entry.addresses[1]);
    memcpy(&ip, entry.addresses[0], sizeof ip);
    CHECK(ip == htonl(INADDR_ANY));
    host = h_nsk_GetHost("192.0.2.1", NULL, 0);
    CHECK(host != NULL);
    memcpy(&entry, host, sizeof entry);
    CHECK(entry.addresses[0] != NULL);
    memcpy(&ip, entry.addresses[0], sizeof ip);
    CHECK(ip == htonl(0xc0000201));
    CHECK(h_sk_GetHostByName("example.invalid") == NULL && h_nsk_GetErrno() == 50);
    CHECK(h_sk_GetHostByName("192.0.2.256") == NULL);

    lo_ai_plain hint = {.family = 2, .type = 2};
    void *result;
    CHECK(plain_h_c_sk_Gai("localhost", "1234", &hint, &result) == 0);
    lo_ai_plain *ai = result;
    CHECK(ai->family == 2 && ai->type == 2 && ai->protocol == 17 && ai->addrlen == 16 &&
          ai->addr->ip == htonl(0x7f000001) && ntohs(ai->addr->port) == 1234 && !ai->next);
    plain_h_c_sk_FreeAi(result);
    hint.flags = 4; // AI_NUMERICHOST
    CHECK(plain_h_c_sk_Gai("0.0.0.0", "48800", &hint, &result) == 0);
    ai = result;
    CHECK(ai->addr->ip == htonl(INADDR_ANY) && ntohs(ai->addr->port) == 48800);
    lo_addr addr = *ai->addr;
    plain_h_c_sk_FreeAi(result);
    int fd = h_nsk_Socket(2, 2, 0);
    CHECK(fd >= 576);
    unsigned port;
    for (port = 48800; port <= 48999; port++) {
        addr.port = htons(port);
        if (plain_h_sk_Bind(fd, &addr, sizeof addr) == 0) break;
        CHECK(h_nsk_GetErrno() == 48);
    }
    CHECK(port <= 48999);
    unsigned len = sizeof addr;
    CHECK(h_nsk_GetSockName(fd, &addr, &len) == 0 &&
          addr.ip == htonl(INADDR_LOOPBACK) && ntohs(addr.port) == port);
    CHECK(h_nsk_Close(fd) == 0);
    lo_ai_nn typed_hint = {.flags = 2, .family = 2, .type = 1};
    CHECK(h_sk_GetAddrInfo("127.0.0.2", "4321", &typed_hint, &result) == 0);
    lo_ai_nn *typed = result;
    CHECK(typed->family == 2 && typed->type == 1 && typed->protocol == 6 &&
          !strcmp(typed->canonname, "127.0.0.2") && typed->addr->ip == htonl(0x7f000002) &&
          ntohs(typed->addr->port) == 4321 && !typed->next);
    h_sk_FreeAddrInfo(result);
    CHECK(h_sk_GetAddrInfo("example.invalid", NULL, NULL, &result) == 4 && result == NULL);
}

int main(void) {
    alarm(10);
    unsetenv("DM_NET_LOOPBACK");
    CHECK(dm_lo_enabled() == DM_GAME_NET_LOOPBACK);
    setenv("DM_NET_LOOPBACK", "0", 1);
    CHECK(h_nsk_Socket(2, 2, 0) == -1 && h_nsk_GetErrno() == 50);
    CHECK(plain_h_sk_Socket3i(2, 1, 0) == -1 && plain_h_sk_Errno() == 50);
    CHECK(h_nifm_Available() == 0);
    setenv("DM_NET_LOOPBACK", "1", 1);
    CHECK(h_nsk_Socket(28, 2, 0) == -1 && h_nsk_GetErrno() == 47);
    CHECK(h_nsk_Socket(2, 3, 0) == -1 && h_nsk_GetErrno() == 44);
    CHECK(h_nifm_Available() == 0);
    test_udp();
    test_tcp();
    test_lookup();
    printf("loopback sockets: %d checks passed\n", checks);
    return 0;
}
