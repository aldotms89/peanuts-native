#pragma once

#include <stdint.h>

int dm_lo_enabled(void);
int dm_lo_socket(int family, int type, int protocol);
int dm_lo_bind(int fd, const void *addr, unsigned len);
int dm_lo_connect(int fd, const void *addr, unsigned len);
int dm_lo_listen(int fd, int backlog);
int dm_lo_accept(int fd, void *addr, unsigned *len);
int dm_lo_close(int fd);
int dm_lo_name(int fd, void *addr, unsigned *len, int peer);
int dm_lo_getopt(int fd, int level, int opt, void *value, unsigned *len);
int dm_lo_setopt(int fd, int level, int opt, const void *value, unsigned len);
int dm_lo_fcntl(int fd, int cmd, long arg);
int dm_lo_shutdown(int fd, int how);
long dm_lo_send(int fd, const void *buf, unsigned long size, int flags, const void *addr, unsigned len);
long dm_lo_recv(int fd, void *buf, unsigned long size, int flags, void *addr, unsigned *len);
void *dm_lo_host(const char *name);
int dm_lo_gai(const char *host, const char *service, const void *hint, void **result, int nn_abi);
