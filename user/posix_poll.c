/* Copyright (C) 2026 NOS contributors */
#include "poll_abi.h"
#include "user.h"
#define POLL_MAX 1024u
/* This is a real readiness query followed by bounded sleep, not a synthetic
 * ready result. Atomic signal-mask replacement for pselect/ppoll is handled
 * separately by the signal ABI; these native helpers never change masks. */
static intptr_t poll_wait(struct nos_pollfd *descriptors, size_t count, int timeout_ms, int gateway)
{
    if (count > POLL_MAX) return -NOS_EINVAL;
    size_t bytes = count * sizeof(*descriptors);
    int rc = nos_user_check(descriptors, bytes, NOS_PROT_READ | NOS_PROT_WRITE);
    if (rc) return rc;
    uint64_t start, now;
    rc = (int)nos_syscall(NOS_SYS_TICKS64, (uintptr_t)&start, 0, 0);
    if (rc) return rc;
    uint64_t duration = timeout_ms < 0 ? UINT64_MAX : ((uint64_t)timeout_ms + 9) / 10;
    for (;;) {
        intptr_t ready = 0;
        for (size_t offset = 0; offset < count;) {
            size_t n = count - offset < NOS_MSG_DATA / sizeof(*descriptors) ?
                count - offset : NOS_MSG_DATA / sizeof(*descriptors);
            struct nos_message message = { .op = NOS_VFS_POLL, .len = n * sizeof(*descriptors) };
            rc = nos_user_copy(message.data, descriptors + offset, message.len);
            if (rc) return rc;
            size_t expected = message.len;
            intptr_t result = nos_rpc(&message);
            if (result < 0) return result;
            if (message.len != expected || (uintptr_t)result > n) return -NOS_EIO;
            rc = nos_user_copy(descriptors + offset, message.data, expected);
            if (rc) return rc;
            ready += result; offset += n;
        }
        if (ready || !timeout_ms) return ready;
        rc = (int)nos_syscall(NOS_SYS_TICKS64, (uintptr_t)&now, 0, 0);
        if (rc) return rc;
        if (duration != UINT64_MAX && now - start >= duration) return 0;
        rc = gateway ? (int)nos_syscall(NOS_SYS_LINUX_WAITCONTROL, 1, 1, 0) :
            (int)nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
        if (rc) return rc;
    }
}
intptr_t nos_poll(struct nos_pollfd *descriptors, size_t count, int timeout_ms)
{ return poll_wait(descriptors, count, timeout_ms, 0); }
intptr_t nos_poll_gateway(struct nos_pollfd *descriptors, size_t count, int timeout_ms)
{ return poll_wait(descriptors, count, timeout_ms, 1); }
static int set_bit(const uintptr_t *set, unsigned fd)
{ return set && (set[fd / (sizeof(uintptr_t) * 8)] >> (fd % (sizeof(uintptr_t) * 8)) & 1); }
static void add_bit(uintptr_t *set, unsigned fd)
{ if (set) set[fd / (sizeof(uintptr_t) * 8)] |= (uintptr_t)1 << (fd % (sizeof(uintptr_t) * 8)); }
static intptr_t select_inner(int count, uintptr_t read_set, uintptr_t write_set, uintptr_t exception_set,
                    int64_t seconds, long nanoseconds, struct nos_pollfd *descriptors, unsigned capacity, int gateway)
{
    if (count < 0 || count > (int)POLL_MAX || seconds < -1 || nanoseconds < 0 || nanoseconds >= 1000000000)
        return -NOS_EINVAL;
    /* Descriptor sets are bounded by the fixed VFS descriptor budget. The
     * full fd_set is copied safely before selecting any referenced bits. */
    uintptr_t input[3][POLL_MAX / (sizeof(uintptr_t) * 8)];
    uintptr_t pointers[3] = {read_set, write_set, exception_set};
    size_t bytes = ((size_t)count + sizeof(uintptr_t) * 8 - 1) / (sizeof(uintptr_t) * 8) * sizeof(uintptr_t);
    for (unsigned i = 0; i < 3; i++) {
        u_memset(input[i], 0, sizeof(input[i]));
        if (!pointers[i]) continue;
        int rc = nos_user_check((void *)pointers[i], bytes, NOS_PROT_WRITE);
        if (rc) return rc;
        rc = nos_user_copy(input[i], (void *)pointers[i], bytes);
        if (rc) return rc;
    }
    unsigned n = 0;
    for (unsigned fd = 0; fd < (unsigned)count; fd++) {
        int events = set_bit(pointers[0] ? input[0] : NULL, fd) ? NOS_POLLIN : 0;
        if (set_bit(pointers[1] ? input[1] : NULL, fd)) events |= NOS_POLLOUT;
        if (set_bit(pointers[2] ? input[2] : NULL, fd)) events |= NOS_POLLPRI;
        if (!events) continue;
        if (n == capacity) return -NOS_EINVAL;
        descriptors[n++] = (struct nos_pollfd){(int)fd, (int16_t)events, 0};
    }
    /* Long timeouts are chunked so the public millisecond int API does not
     * turn a valid POSIX timespec into a wrapped negative/infinite timeout. */
    if (seconds > (INT64_MAX - nanoseconds / 1000000 - 1) / 1000) return -NOS_EOVERFLOW;
    uint64_t left = seconds < 0 ? UINT64_MAX : (uint64_t)seconds * 1000 + (nanoseconds + 999999) / 1000000;
    for (;;) {
        int timeout = left == UINT64_MAX ? -1 : left > INT32_MAX ? INT32_MAX : (int)left;
        intptr_t result = poll_wait(descriptors, n, timeout, gateway);
        if (result < 0) return result;
        if (result || left == UINT64_MAX || left <= INT32_MAX) break;
        left -= INT32_MAX;
    }
    for (unsigned i = 0; i < 3; i++) u_memset(input[i], 0, sizeof(input[i]));
    intptr_t ready = 0;
    for (unsigned i = 0; i < n; i++) {
        unsigned fd = (unsigned)descriptors[i].fd, events = (unsigned short)descriptors[i].revents;
        if (events & NOS_POLLNVAL) return -NOS_EBADF;
        /* EOF is readable; a broken write end reports an error and select
         * marks it writable so the ensuing write can return EPIPE. */
        if (pointers[0] && descriptors[i].events & NOS_POLLIN && events & (NOS_POLLIN | NOS_POLLHUP | NOS_POLLERR))
            { add_bit(input[0], fd); ready++; }
        if (pointers[1] && descriptors[i].events & NOS_POLLOUT && events & (NOS_POLLOUT | NOS_POLLERR))
            { add_bit(input[1], fd); ready++; }
        if (pointers[2] && events & NOS_POLLPRI) { add_bit(input[2], fd); ready++; }
    }
    for (unsigned i = 0; i < 3; i++) if (pointers[i]) {
        int rc = nos_user_copy((void *)pointers[i], input[i], bytes);
        if (rc) return rc;
    }
    return ready;
}
static intptr_t select_wait(int count, uintptr_t read_set, uintptr_t write_set, uintptr_t exception_set,
                            int64_t seconds, long nanoseconds, int gateway)
{
    if (count < 0 || count > (int)POLL_MAX) return -NOS_EINVAL;
    struct nos_pollfd local[32];
    if (count <= 32) return select_inner(count, read_set, write_set, exception_set,
                                       seconds, nanoseconds, local, 32, gateway);
    size_t bytes = (size_t)count * sizeof(*local);
    intptr_t allocation = nos_syscall(NOS_SYS_MMAP, NOS_PROT_READ | NOS_PROT_WRITE, bytes, 0);
    if (nos_pointer_is_error(allocation)) return allocation;
    intptr_t result = select_inner(count, read_set, write_set, exception_set, seconds,
                                  nanoseconds, (struct nos_pollfd *)(uintptr_t)allocation, (unsigned)count, gateway);
    (void)nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)allocation, bytes, 0);
    return result;
}
intptr_t nos_select(int count, uintptr_t read_set, uintptr_t write_set, uintptr_t exception_set,
                    int64_t seconds, long nanoseconds)
{ return select_wait(count, read_set, write_set, exception_set, seconds, nanoseconds, 0); }
intptr_t nos_select_gateway(int count, uintptr_t read_set, uintptr_t write_set, uintptr_t exception_set,
                            int64_t seconds, long nanoseconds)
{ return select_wait(count, read_set, write_set, exception_set, seconds, nanoseconds, 1); }
