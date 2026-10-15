/* Copyright (C) 2026 NOS contributors */
#define _GNU_SOURCE
#include "abi.h"
#include "user.h"
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <sys/mman.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>

#define NOS_FUTEX_WAIT 0
#define NOS_FUTEX_WAKE 1
#define NOS_FUTEX_PRIVATE 128
#define NOS_FUTEX_CLOCK_REALTIME 256
static int fd_argument(long value)
{ return value >= INT_MIN && value <= INT_MAX; }
static long vector_io(long fd, long pointer, long count, int write)
{
    struct iovec vectors[16];
    if (!fd_argument(fd) || count < 0 || count > 16) return -NOS_EINVAL;
    int rc = nos_user_copy(vectors, (void *)(uintptr_t)pointer, (size_t)count * sizeof(*vectors));
    if (rc) return rc;
    size_t total = 0;
    for (long i = 0; i < count; i++) {
        if (vectors[i].iov_len > (size_t)LONG_MAX - total) return -NOS_EINVAL;
        total += vectors[i].iov_len;
        rc = nos_user_check(vectors[i].iov_base, vectors[i].iov_len,
            write ? NOS_PROT_READ : NOS_PROT_WRITE);
        if (rc) return rc;
    }
    if (!count) return write ? nos_posix_write((int)fd, NULL, 0) : nos_posix_read((int)fd, NULL, 0);
    long done = 0;
    for (long i = 0; i < count; i++) {
        intptr_t result;
        if (done) result = write ? nos_write_nowait((int)fd, vectors[i].iov_base, vectors[i].iov_len) :
            nos_read_nowait((int)fd, vectors[i].iov_base, vectors[i].iov_len);
        else result = write ? nos_posix_write((int)fd, vectors[i].iov_base, vectors[i].iov_len) :
            nos_posix_read((int)fd, vectors[i].iov_base, vectors[i].iov_len);
        if (result < 0) return done ? done : result;
        done += result;
        if ((size_t)result < vectors[i].iov_len) break;
    }
    return done;
}
/* Signal-mask state is retained per thread. Installing handlers and delivering
 * signals remain unsupported and return errors; masking itself is real state. */
static struct { long tid; uint64_t mask; } masks[NOS_TASKS];
static long signal_mask(long how, long set, long old, long bytes)
{
    if (bytes != 8) return -NOS_EINVAL;
    if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) return -NOS_EINVAL;
    long tid = nos_syscall(NOS_SYS_GETPID, 0, 0, 0);
    unsigned index = (unsigned)tid % NOS_TASKS;
    uint64_t previous = masks[index].tid == tid ? masks[index].mask : 0;
    uint64_t requested = 0;
    int rc;
    if (set && (rc = nos_user_copy(&requested, (void *)(uintptr_t)set, sizeof(requested)))) return rc;
    if (old && (rc = nos_user_copy((void *)(uintptr_t)old, &previous, sizeof(previous)))) return rc;
    if (set) {
        requested &= ~((UINT64_C(1) << (SIGKILL - 1)) | (UINT64_C(1) << (SIGSTOP - 1)));
        uint64_t next = how == SIG_SETMASK ? requested : how == SIG_BLOCK ?
            previous | requested : previous & ~requested;
        masks[index].tid = tid; masks[index].mask = next;
    }
    return 0;
}
static long futex_call(long address, long operation, long value, long timeout, long target, long compare)
{
    unsigned op = (unsigned)operation & ~NOS_FUTEX_PRIVATE;
    if (op == NOS_FUTEX_WAKE) {
        if (value < 0 || value > INT_MAX) return -NOS_EINVAL;
        return nos_syscall(NOS_SYS_FUTEX_WAKE, (uintptr_t)address, (uintptr_t)value, 0);
    }
    if (op == 3 || op == 4) {
        if (value < 0 || value > INT_MAX || timeout < 0 || timeout > INT_MAX) return -NOS_EINVAL;
        struct nos_futex_requeue request = { .source = (uintptr_t)address, .target = (uintptr_t)target,
            .wake_count = (uint32_t)value, .requeue_count = (uint32_t)timeout,
            .compare = (uint32_t)compare, .flags = op == 4 };
        return nos_syscall(NOS_SYS_FUTEX_REQUEUE, (uintptr_t)&request, 0, 0);
    }
    if (op != NOS_FUTEX_WAIT) return -NOS_ENOTSUP;
    uintptr_t ticks = 0;
    if (timeout) {
        struct timespec duration;
        int rc = nos_user_copy(&duration, (void *)(uintptr_t)timeout, sizeof(duration));
        if (rc) return rc;
        if (duration.tv_sec < 0 || duration.tv_nsec < 0 || duration.tv_nsec >= 1000000000L)
            return -NOS_EINVAL;
        if ((uint64_t)duration.tv_sec > (UINTPTR_MAX - 100) / 100) return -NOS_EOVERFLOW;
        ticks = (uintptr_t)duration.tv_sec * 100 + (uintptr_t)(duration.tv_nsec + 9999999) / 10000000;
        if (!ticks) return -NOS_ETIMEDOUT;
    }
    return nos_syscall(NOS_SYS_FUTEX_WAIT, (uintptr_t)address, (uint32_t)value, ticks);
}
static long sleep_request(uintptr_t request, uintptr_t remaining, int absolute)
{
    struct timespec duration;
    int rc = nos_user_copy(&duration, (void *)request, sizeof(duration));
    if (rc) return rc;
    if (duration.tv_sec < 0 || duration.tv_nsec < 0 || duration.tv_nsec >= 1000000000L) return -NOS_EINVAL;
    uint64_t seconds = (uint64_t)duration.tv_sec;
    int64_t nanoseconds = duration.tv_nsec;
    if (absolute) {
        struct nos_time_value now;
        rc = nos_clock_snapshot(&now);
        if (rc) return rc;
        if (duration.tv_sec < now.seconds ||
            (duration.tv_sec == now.seconds && (uint64_t)duration.tv_nsec <= now.nanoseconds)) return 0;
        seconds -= (uint64_t)now.seconds;
        nanoseconds -= now.nanoseconds;
        if (nanoseconds < 0) { seconds--; nanoseconds += 1000000000L; }
    }
    if (seconds > (UINT64_MAX - 100) / 100) return -NOS_EOVERFLOW;
    uint64_t ticks = seconds * 100 + (uint64_t)(nanoseconds + 9999999) / 10000000;
    uint64_t left = 0;
    rc = nos_sleep_ticks64(ticks, &left);
    if (rc == -NOS_EINTR && remaining && !absolute) {
        struct nos_time_value residual;
        nos_time_from_ticks(left, &residual);
        struct timespec value = { .tv_sec = (time_t)residual.seconds, .tv_nsec = residual.nanoseconds };
        int copy = nos_user_copy((void *)remaining, &value, sizeof(value));
        if (copy) return copy;
    }
    return rc;
}
long nos_musl_syscall(long n, long a, long b, long c, long d, long e, long f)
{
    (void)e;
    switch (n) {
    case SYS_write:
        if (!fd_argument(a) || c < 0) return -NOS_EINVAL;
        return nos_posix_write((int)a, (const void *)(uintptr_t)b, (size_t)c);
    case SYS_read:
        if (!fd_argument(a) || c < 0) return -NOS_EINVAL;
        return nos_posix_read((int)a, (void *)(uintptr_t)b, (size_t)c);
    case SYS_writev: return vector_io(a, b, c, 1);
    case SYS_readv: return vector_io(a, b, c, 0);
    case SYS_openat: {
        if (a != AT_FDCWD) return -NOS_ENOTSUP;
        if (c < 0 || (unsigned long)c > UINT_MAX) return -NOS_EINVAL;
        unsigned flags = (unsigned)c;
        unsigned supported = O_ACCMODE | O_CREAT | O_TRUNC | O_LARGEFILE | O_APPEND | O_NONBLOCK;
        if (flags & ~supported) return -NOS_ENOTSUP;
        unsigned mode = flags & O_ACCMODE;
        if (flags & O_CREAT) mode |= NOS_O_CREAT;
        if (flags & O_TRUNC) mode |= NOS_O_TRUNC;
        if (flags & O_APPEND) mode |= NOS_O_APPEND;
        if (flags & O_NONBLOCK) mode |= NOS_O_NONBLOCK;
        return nos_open((const char *)(uintptr_t)b, mode);
    }
    case SYS_close: return fd_argument(a) ? nos_close((int)a) : -NOS_EBADF;
    case SYS_lseek: return fd_argument(a) && fd_argument(c) ? nos_seek((int)a, b, (int)c) : -NOS_EINVAL;
    case SYS_fstat: {
        if (!fd_argument(a)) return -NOS_EBADF;
        struct nos_stat info;
        long rc = nos_stat_fd((int)a, &info);
        if (rc) return rc;
        struct stat st;
        u_memset(&st, 0, sizeof(st));
        st.st_mode = info.mode; st.st_size = (off_t)info.size;
        st.st_blksize = 128; st.st_nlink = 1;
        return nos_user_copy((void *)(uintptr_t)b, &st, sizeof(st));
    }
    case SYS_dup: return fd_argument(a) ? nos_dup((int)a) : -NOS_EBADF;
    case SYS_dup3:
        if (!fd_argument(a) || !fd_argument(b) || a == b || c) return -NOS_EINVAL;
        return nos_dup2((int)a, (int)b);
    case SYS_pipe2:
        if (b & ~O_NONBLOCK) return -NOS_ENOTSUP;
        return nos_pipe2((int *)(uintptr_t)a, b & O_NONBLOCK ? NOS_O_NONBLOCK : 0);
    case SYS_fcntl: {
        if (!fd_argument(a)) return -NOS_EBADF;
        if (b == F_DUPFD) {
            if (c < 0 || (unsigned long)c > UINT_MAX) return -NOS_EINVAL;
            return nos_dup_min((int)a, (unsigned)c);
        }
        int flags = nos_getflags((int)a);
        if (flags < 0) return flags;
        if (b == F_GETFL) return (flags & 3) | (flags & NOS_O_APPEND ? O_APPEND : 0) |
            (flags & NOS_O_NONBLOCK ? O_NONBLOCK : 0);
        if (b == F_SETFL) {
            if ((unsigned long)c & ~(unsigned long)(O_ACCMODE | O_APPEND | O_NONBLOCK)) return -NOS_ENOTSUP;
            return nos_setflags((int)a, (c & O_APPEND ? NOS_O_APPEND : 0) |
                (c & O_NONBLOCK ? NOS_O_NONBLOCK : 0));
        }
        if (b == F_GETFD || (b == F_SETFD && !c)) return 0;
        return -NOS_ENOTSUP;
    }
    case SYS_mkdirat:
        if (a != AT_FDCWD) return -NOS_ENOTSUP;
        return nos_mkdir((const char *)(uintptr_t)b);
    case SYS_unlinkat:
        if (a != AT_FDCWD) return -NOS_ENOTSUP;
        if (c == AT_REMOVEDIR) return nos_rmdir((const char *)(uintptr_t)b);
        if (c) return -NOS_EINVAL;
        return nos_unlink((const char *)(uintptr_t)b);
#ifdef SYS_renameat
    case SYS_renameat:
        if (a != AT_FDCWD || c != AT_FDCWD) return -NOS_ENOTSUP;
        return nos_rename((const char *)(uintptr_t)b, (const char *)(uintptr_t)d);
#endif
#ifdef SYS_renameat2
    case SYS_renameat2:
        if (a != AT_FDCWD || c != AT_FDCWD || e) return -NOS_ENOTSUP;
        return nos_rename((const char *)(uintptr_t)b, (const char *)(uintptr_t)d);
#endif
    case SYS_fsync: case SYS_fdatasync: {
        struct nos_stat st;
        int rc = nos_stat_fd((int)a, &st);
        return rc ? rc : nos_sync();
    }
    case SYS_sync: return nos_sync();
    case SYS_brk: {
        long old = nos_syscall(NOS_SYS_BRK, 0, 0, 0);
        if (!a) return old;
        long rc = nos_syscall(NOS_SYS_BRK, (uintptr_t)a, 0, 0);
        return rc < 0 ? old : rc;
    }
    case SYS_mmap:
        if (b <= 0) return -NOS_EINVAL;
        if (a || d != (MAP_PRIVATE | MAP_ANONYMOUS) || f) return -NOS_ENOTSUP;
        return nos_syscall(NOS_SYS_MMAP, (uintptr_t)c, (uintptr_t)b, 0);
    case SYS_munmap:
        if (b <= 0 || (uintptr_t)a % NOS_PAGE_SIZE) return -NOS_EINVAL;
        return nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)a, (uintptr_t)b, 0);
    case SYS_mprotect:
        if (b <= 0 || (uintptr_t)a % NOS_PAGE_SIZE) return -NOS_EINVAL;
        return nos_syscall(NOS_SYS_MPROTECT, (uintptr_t)a, (uintptr_t)c, (uintptr_t)b);
    case SYS_getpid: return nos_syscall(NOS_SYS_GROUP_ID, 0, 0, 0);
    case SYS_gettid: return nos_syscall(NOS_SYS_GETPID, 0, 0, 0);
    case SYS_set_tid_address: return nos_syscall(NOS_SYS_SET_TID_ADDRESS, (uintptr_t)a, 0, 0);
    case SYS_futex: return futex_call(a, b, c, d, e, f);
    case SYS_sched_yield: return nos_syscall(NOS_SYS_YIELD, 0, 0, 0);
    case SYS_rt_sigprocmask: return signal_mask(a, b, c, d);
    case SYS_getuid: case SYS_geteuid: case SYS_getgid: case SYS_getegid:
        return -NOS_ENOTSUP;
    case SYS_clock_gettime: {
        if (a == CLOCK_REALTIME) return -NOS_ENOTSUP;
        if (a != CLOCK_MONOTONIC) return -NOS_EINVAL;
        struct timespec time;
        struct nos_time_value snapshot;
        int rc = nos_clock_snapshot(&snapshot);
        if (rc) return rc;
        time.tv_sec = (time_t)snapshot.seconds; time.tv_nsec = snapshot.nanoseconds;
        return nos_user_copy((void *)(uintptr_t)b, &time, sizeof(time));
    }
    case SYS_clock_getres: {
        if (a != CLOCK_MONOTONIC) return -NOS_ENOTSUP;
        struct timespec resolution = { 0, 10000000 };
        return b ? nos_user_copy((void *)(uintptr_t)b, &resolution, sizeof(resolution)) : 0;
    }
    case SYS_nanosleep: return sleep_request((uintptr_t)a, (uintptr_t)b, 0);
    case SYS_clock_nanosleep:
        if (b & ~TIMER_ABSTIME) return -NOS_EINVAL;
        if (a != CLOCK_MONOTONIC && !(a == CLOCK_REALTIME && !b)) return -NOS_ENOTSUP;
        return sleep_request((uintptr_t)c, (uintptr_t)d, !!(b & TIMER_ABSTIME));
    case SYS_membarrier:
        /* Linux MEMBARRIER_CMD_QUERY can truthfully return no commands. */
        return a == 0 ? 0 : -NOS_ENOTSUP;
    case SYS_ioctl: {
        struct nos_stat st;
        if (!fd_argument(a)) return -NOS_EBADF;
        int rc = nos_stat_fd((int)a, &st);
        return rc ? rc : -25;
    }
    case SYS_exit: nos_process_exit((int)a);
    case SYS_exit_group:
        nos_syscall(NOS_SYS_EXIT_GROUP, (uintptr_t)a, 0, 0); for (;;) { }
    default: return -NOS_ENOSYS;
    }
}

struct clone_start { int (*function)(void *); void *argument; };
static void clone_start(uintptr_t pointer)
{
    struct clone_start *start = (void *)pointer;
    int (*function)(void *) = start->function;
    void *argument = start->argument;
    int status = function(argument);
    nos_syscall(NOS_SYS_EXIT, (uintptr_t)status, 0, 0);
    for (;;) { }
}
/* musl's architecture __clone assembly issues Linux syscalls directly. A strong
 * port definition keeps that archive member out of the linked NOS program. */
int __clone(int (*function)(void *), void *stack, int flags, void *argument,
    int *parent_tid, void *tls, int *clear_tid)
{
    const unsigned required = 0x100u | 0x200u | 0x400u | 0x800u | 0x10000u |
        0x40000u | 0x80000u | 0x100000u | 0x200000u;
    const unsigned permitted = required | 0x400000u;
    if (((unsigned)flags & required) != required || ((unsigned)flags & ~permitted))
        return -NOS_ENOTSUP;
    uintptr_t top = (uintptr_t)stack & ~(uintptr_t)15;
    if (top < 4096) return -NOS_EINVAL;
    struct clone_start *start = (void *)(top - 32);
    struct clone_start value = { function, argument };
    int rc = nos_user_copy(start, &value, sizeof(value));
    if (rc) return rc;
    struct nos_thread_args request = { .entry = (uintptr_t)clone_start,
        .arg = (uintptr_t)start, .stack = top - 48, .stack_size = 16,
        .tls = (uintptr_t)tls, .clear_tid = (uintptr_t)clear_tid,
        .parent_tid = (uintptr_t)parent_tid };
    return (int)nos_syscall(NOS_SYS_THREAD_CREATE, (uintptr_t)&request, UINTPTR_MAX, 0);
}
long __syscall_cp_asm(volatile int *cancel, long number, long a, long b, long c,
    long d, long e, long f)
{
    /* Cancellation requires signal delivery, which this port reports unsupported. */
    if (*cancel) return -NOS_ENOTSUP;
    return nos_musl_syscall(number, a, b, c, d, e, f);
}
void __unmapself(void *base, size_t size)
{
    nos_syscall(NOS_SYS_EXIT, 0, (uintptr_t)base, size);
    for (;;) { }
}

#include <pthread.h>
int pthread_cancel(pthread_t thread)
{
    (void)thread;
    return NOS_ENOTSUP;
}
