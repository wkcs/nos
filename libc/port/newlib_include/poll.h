/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_NEWLIB_POLL_H
#define NOS_NEWLIB_POLL_H
#include <sys/types.h>
#include <time.h>
#include <signal.h>
typedef unsigned long nfds_t;
struct pollfd { int fd; short events, revents; };
#define POLLIN 0x001
#define POLLPRI 0x002
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020
#define POLLRDNORM 0x040
#define POLLRDBAND 0x080
#define POLLWRNORM 0x100
#define POLLWRBAND 0x200
#ifdef __cplusplus
extern "C" {
#endif
int poll(struct pollfd *, nfds_t, int);
int ppoll(struct pollfd *, nfds_t, const struct timespec *, const sigset_t *);
#ifdef __cplusplus
}
#endif
#endif
