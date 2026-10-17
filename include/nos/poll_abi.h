/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_POLL_ABI_H
#define NOS_POLL_ABI_H
#include "abi.h"
struct nos_pollfd { int fd; int16_t events, revents; };
_Static_assert(sizeof(struct nos_pollfd) == 8 && offsetof(struct nos_pollfd, revents) == 6,
               "poll descriptor wire layout");
#define NOS_POLLIN 0x001
#define NOS_POLLPRI 0x002
#define NOS_POLLOUT 0x004
#define NOS_POLLERR 0x008
#define NOS_POLLHUP 0x010
#define NOS_POLLNVAL 0x020
#define NOS_POLLRDNORM 0x040
#define NOS_POLLWRNORM 0x100
intptr_t nos_poll(struct nos_pollfd *descriptors, size_t count, int timeout_ms);
intptr_t nos_poll_gateway(struct nos_pollfd *descriptors, size_t count, int timeout_ms);
intptr_t nos_select(int count, uintptr_t read_set, uintptr_t write_set, uintptr_t exception_set,
                    int64_t seconds, long nanoseconds);
intptr_t nos_select_gateway(int count, uintptr_t read_set, uintptr_t write_set, uintptr_t exception_set,
                            int64_t seconds, long nanoseconds);
#endif
