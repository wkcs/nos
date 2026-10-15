/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
#include "user.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <reent.h>

/* Every shared address space initializes its allocator lock before running an
 * application entry. A futex protects the publication if startup is preempted. */
static int malloc_mutex = -1;
static intptr_t malloc_owner = -1;
static unsigned malloc_depth;
static int malloc_poisoned;
void nos_newlib_init(void)
{
    int expected = -1;
    if (__atomic_compare_exchange_n(&malloc_mutex, &expected, -2, 0,
            __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        int mutex = (int)nos_syscall(NOS_SYS_MUTEX_CREATE, 0, 0, 0);
        __atomic_store_n(&malloc_mutex, mutex, __ATOMIC_RELEASE);
        nos_syscall(NOS_SYS_FUTEX_WAKE, (uintptr_t)&malloc_mutex, NOS_TASKS, 0);
    } else {
        while (__atomic_load_n(&malloc_mutex, __ATOMIC_ACQUIRE) == -2)
            nos_syscall(NOS_SYS_FUTEX_WAIT, (uintptr_t)&malloc_mutex, (uint32_t)-2, 0);
    }
    if (__atomic_load_n(&malloc_mutex, __ATOMIC_ACQUIRE) < 0) nos_syscall(NOS_SYS_EXIT, NOS_ENOMEM, 0, 0);
}
void __malloc_lock(struct _reent *reent)
{
    (void)reent;
    intptr_t tid = nos_syscall(NOS_SYS_GETPID, 0, 0, 0);
    if (__atomic_load_n(&malloc_owner, __ATOMIC_ACQUIRE) == tid) { malloc_depth++; return; }
    nos_newlib_init();
    if (__atomic_load_n(&malloc_poisoned, __ATOMIC_ACQUIRE) ||
        nos_syscall(NOS_SYS_MUTEX_LOCK, (uintptr_t)__atomic_load_n(&malloc_mutex, __ATOMIC_ACQUIRE), 0, 0))
        nos_syscall(NOS_SYS_EXIT, NOS_EIO, 0, 0);
    /* Kernel owner-death unlock does not repair malloc metadata. Fail closed
     * rather than using an allocator interrupted in the middle of an update. */
    if (__atomic_load_n(&malloc_owner, __ATOMIC_ACQUIRE) != -1) {
        __atomic_store_n(&malloc_poisoned, 1, __ATOMIC_RELEASE);
        nos_syscall(NOS_SYS_MUTEX_UNLOCK, (uintptr_t)malloc_mutex, 0, 0);
        nos_syscall(NOS_SYS_EXIT, NOS_EIO, 0, 0);
    }
    malloc_depth = 1; __atomic_store_n(&malloc_owner, tid, __ATOMIC_RELEASE);
}
void __malloc_unlock(struct _reent *reent)
{
    (void)reent;
    if (!malloc_depth || __atomic_load_n(&malloc_owner, __ATOMIC_ACQUIRE) != nos_syscall(NOS_SYS_GETPID, 0, 0, 0))
        nos_syscall(NOS_SYS_EXIT, NOS_EIO, 0, 0);
    if (--malloc_depth) return;
    __atomic_store_n(&malloc_owner, -1, __ATOMIC_RELEASE);
    nos_syscall(NOS_SYS_MUTEX_UNLOCK, (uintptr_t)malloc_mutex, 0, 0);
}
void nos_libc_task_cleanup(void)
{
    struct _reent *reent = _REENT;
    if (!reent) return;
    /* Newlib skips _impure_ptr and its cleanup callback owns process-wide
     * stdio. Reclaim only this thread's allocation caches through a copy. */
    struct _reent copy = *reent;
    _REENT_CLEANUP(&copy) = NULL;
    _reclaim_reent(&copy);
    u_memset(reent, 0, sizeof(*reent));
}
static intptr_t result(intptr_t rc)
{ if (rc < 0) { errno = (int)-rc; return -1; } return rc; }
int _open(const char *path, int flags, ...)
{
    unsigned supported = O_ACCMODE | O_CREAT | O_TRUNC;
#ifdef O_APPEND
    supported |= O_APPEND | O_NONBLOCK;
#endif
    if ((unsigned)flags & ~supported) { errno = ENOTSUP; return -1; }
    unsigned f = flags & O_ACCMODE;
    if (flags & O_CREAT) f |= NOS_O_CREAT;
    if (flags & O_TRUNC) f |= NOS_O_TRUNC;
#ifdef O_APPEND
    if (flags & O_APPEND) f |= NOS_O_APPEND;
    if (flags & O_NONBLOCK) f |= NOS_O_NONBLOCK;
#endif
    return (int)result(nos_open(path, f));
}
int _close(int fd) { return (int)result(nos_close(fd)); }
_ssize_t _read(int fd, void *buf, size_t n) { return (_ssize_t)result(nos_posix_read(fd, buf, n)); }
_ssize_t _write(int fd, const void *buf, size_t n) { return (_ssize_t)result(nos_posix_write(fd, buf, n)); }
off_t _lseek(int fd, off_t offset, int whence) { return (off_t)result(nos_seek(fd, offset, whence)); }
int _fstat(int fd, struct stat *st)
{
    struct nos_stat info;
    intptr_t rc = nos_stat_fd(fd, &info);
    if (rc) return (int)result(rc);
    struct stat value;
    u_memset(&value, 0, sizeof(value));
    value.st_mode = info.mode; value.st_size = (off_t)info.size;
    value.st_blksize = 128; value.st_nlink = 1;
    return (int)result(nos_user_copy(st, &value, sizeof(value)));
}
int _isatty(int fd)
{
    struct nos_stat info;
    intptr_t rc = nos_stat_fd(fd, &info);
    if (rc < 0) { errno = (int)-rc; return 0; }
    if ((info.mode & S_IFMT) == S_IFCHR) return 1;
    errno = ENOTTY; return 0;
}
void *_sbrk(ptrdiff_t increment)
{
    uintptr_t old = (uintptr_t)nos_syscall(NOS_SYS_BRK, 0, 0, 0);
    uintptr_t next = old + (uintptr_t)increment;
    if ((increment > 0 && next < old) || (increment < 0 && next > old) ||
        nos_pointer_is_error(nos_syscall(NOS_SYS_BRK, next, 0, 0))) { errno = ENOMEM; return (void *)-1; }
    return (void *)old;
}
int _getpid(void) { return (int)nos_syscall(NOS_SYS_GROUP_ID, 0, 0, 0); }
int _kill(int pid, int signal) { (void)pid; (void)signal; errno = ENOSYS; return -1; }
void _exit(int status)
{
    nos_libc_task_cleanup();
    nos_syscall(NOS_SYS_EXIT_GROUP, (uintptr_t)status, 0, 0);
    for (;;) { }
}
int _gettimeofday(struct timeval *tv, void *zone)
{
    (void)tv; (void)zone;
    /* A boot counter is monotonic time, not a wall clock. */
    errno = ENOTSUP; return -1;
}
int _unlink(const char *path) { return (int)result(nos_unlink(path)); }
int _rename(const char *old_path, const char *new_path)
{ return (int)result(nos_rename(old_path, new_path)); }
int _mkdir(const char *path, mode_t mode)
{ (void)mode; return (int)result(nos_mkdir(path)); }
int rmdir(const char *path) { return (int)result(nos_rmdir(path)); }
int dup(int fd) { return (int)result(nos_dup(fd)); }
int dup2(int fd, int target) { return (int)result(nos_dup2(fd, target)); }
int pipe(int descriptors[2]) { return (int)result(nos_pipe(descriptors)); }

#include <time.h>
int clock_gettime(clockid_t clock, struct timespec *time)
{
    if (clock != CLOCK_MONOTONIC) { errno = ENOTSUP; return -1; }
    struct nos_time_value snapshot;
    int rc = nos_clock_snapshot(&snapshot);
    if (rc) return (int)result(rc);
    struct timespec value = { .tv_sec = (time_t)snapshot.seconds, .tv_nsec = snapshot.nanoseconds };
    return (int)result(nos_user_copy(time, &value, sizeof(value)));
}
int nanosleep(const struct timespec *request, struct timespec *remaining)
{
    struct timespec value;
    int rc = nos_user_copy(&value, request, sizeof(value));
    if (rc) return (int)result(rc);
    if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000L) {
        errno = EINVAL; return -1;
    }
    if ((uint64_t)value.tv_sec > (UINT64_MAX - 100) / 100) { errno = EOVERFLOW; return -1; }
    uint64_t ticks = (uint64_t)value.tv_sec * 100 + (uint64_t)(value.tv_nsec + 9999999) / 10000000;
    uint64_t left = 0;
    rc = nos_sleep_ticks64(ticks, &left);
    if (rc == -NOS_EINTR && remaining) {
        struct nos_time_value snapshot;
        nos_time_from_ticks(left, &snapshot);
        struct timespec residual = { .tv_sec = (time_t)snapshot.seconds, .tv_nsec = snapshot.nanoseconds };
        int copy = nos_user_copy(remaining, &residual, sizeof(residual));
        if (copy) return (int)result(copy);
    }
    return (int)result(rc);
}

#ifdef NOS_NEWLIB_DYNAMIC
#include <sys/lock.h>
/* _impure_ptr remains the bootstrap object. The binding slot is never read by
 * libc; TLS_GET retrieves the current task's actual pointer on every CPU. */
static uintptr_t reent_binding;
struct _reent *__getreent(void)
{
    uintptr_t pointer = (uintptr_t)nos_syscall(NOS_SYS_TLS_GET, 0, 0, 0);
    return pointer ? (struct _reent *)pointer : _impure_ptr;
}
int nos_newlib_bind_reent(struct _reent *reent)
{
    return (int)nos_syscall(NOS_SYS_TLS_BIND, (uintptr_t)&reent_binding, (uintptr_t)reent, 0);
}
struct __lock { int owner; unsigned depth, used; };
struct __lock __lock___sfp_recursive_mutex = { .used = 1 };
struct __lock __lock___atexit_recursive_mutex = { .used = 1 };
struct __lock __lock___at_quick_exit_mutex = { .used = 1 };
struct __lock __lock___malloc_recursive_mutex = { .used = 1 };
struct __lock __lock___env_recursive_mutex = { .used = 1 };
struct __lock __lock___tz_mutex = { .used = 1 };
struct __lock __lock___dd_hash_mutex = { .used = 1 };
struct __lock __lock___arc4random_mutex = { .used = 1 };
static struct __lock dynamic_locks[64];
static void lock_failure(void)
{
    nos_syscall(NOS_SYS_EXIT, NOS_EIO, 0, 0);
    for (;;) { }
}
void __retarget_lock_init(_LOCK_T *lock)
{
    for (unsigned i = 0; i < sizeof(dynamic_locks) / sizeof(dynamic_locks[0]); i++) {
        unsigned expected = 0;
        if (__atomic_compare_exchange_n(&dynamic_locks[i].used, &expected, 1, 0,
                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            dynamic_locks[i].owner = 0; dynamic_locks[i].depth = 0;
            *lock = &dynamic_locks[i]; return;
        }
    }
    lock_failure();
}
void __retarget_lock_init_recursive(_LOCK_T *lock) { __retarget_lock_init(lock); }
void __retarget_lock_close(_LOCK_T lock)
{
    if (!lock || __atomic_load_n(&lock->owner, __ATOMIC_ACQUIRE)) lock_failure();
    for (unsigned i = 0; i < sizeof(dynamic_locks) / sizeof(dynamic_locks[0]); i++)
        if (lock == &dynamic_locks[i]) {
            __atomic_store_n(&lock->used, 0, __ATOMIC_RELEASE); return;
        }
    lock_failure();
}
void __retarget_lock_close_recursive(_LOCK_T lock) { __retarget_lock_close(lock); }
static int lock_try(_LOCK_T lock, int recursive)
{
    if (!lock || !__atomic_load_n(&lock->used, __ATOMIC_ACQUIRE)) lock_failure();
    int owner = (int)((unsigned)nos_syscall(NOS_SYS_GETPID, 0, 0, 0) + 1u), expected = 0;
    if (__atomic_compare_exchange_n(&lock->owner, &expected, owner, 0,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) { lock->depth = 1; return 1; }
    if (recursive && expected == owner) { lock->depth++; return 1; }
    return 0;
}
int __retarget_lock_try_acquire(_LOCK_T lock) { return lock_try(lock, 0); }
int __retarget_lock_try_acquire_recursive(_LOCK_T lock) { return lock_try(lock, 1); }
static void lock_acquire(_LOCK_T lock, int recursive)
{
    while (!lock_try(lock, recursive)) {
        int owner = __atomic_load_n(&lock->owner, __ATOMIC_RELAXED);
        if (!owner) continue;
        intptr_t rc = nos_syscall(NOS_SYS_FUTEX_WAIT, (uintptr_t)&lock->owner, (uint32_t)owner, 0);
        if (rc < 0 && rc != -NOS_EAGAIN && rc != -NOS_EINTR) lock_failure();
    }
}
void __retarget_lock_acquire(_LOCK_T lock) { lock_acquire(lock, 0); }
void __retarget_lock_acquire_recursive(_LOCK_T lock) { lock_acquire(lock, 1); }
void __retarget_lock_release(_LOCK_T lock)
{
    int owner = (int)((unsigned)nos_syscall(NOS_SYS_GETPID, 0, 0, 0) + 1u);
    if (!lock || __atomic_load_n(&lock->owner, __ATOMIC_RELAXED) != owner || !lock->depth) lock_failure();
    if (--lock->depth) return;
    __atomic_store_n(&lock->owner, 0, __ATOMIC_RELEASE);
    nos_syscall(NOS_SYS_FUTEX_WAKE, (uintptr_t)&lock->owner, 1, 0);
}
void __retarget_lock_release_recursive(_LOCK_T lock) { __retarget_lock_release(lock); }
#endif

#include <stdarg.h>
static int do_fcntl(int fd, int command, int argument)
{
    if (command == F_DUPFD) return (int)result(nos_dup_min(fd, (unsigned)argument));
    int flags = nos_getflags(fd);
    if (flags < 0) return (int)result(flags);
    if (command == F_GETFL) return (flags & 3) |
        (flags & NOS_O_APPEND ? O_APPEND : 0) | (flags & NOS_O_NONBLOCK ? O_NONBLOCK : 0);
    if (command == F_SETFL) {
        if ((unsigned)argument & ~(O_ACCMODE | O_APPEND | O_NONBLOCK)) { errno = ENOTSUP; return -1; }
        return (int)result(nos_setflags(fd, (argument & O_APPEND ? NOS_O_APPEND : 0) |
            (argument & O_NONBLOCK ? NOS_O_NONBLOCK : 0)));
    }
    if (command == F_GETFD || (command == F_SETFD && !argument)) return 0;
    errno = ENOTSUP; return -1;
}
int _fcntl(int fd, int command, ...)
{
    int argument = 0;
    if (command == F_DUPFD || command == F_SETFL || command == F_SETFD) {
        va_list args; va_start(args, command); argument = va_arg(args, int); va_end(args);
    }
    return do_fcntl(fd, command, argument);
}
int fcntl(int fd, int command, ...)
{
    int argument = 0;
    if (command == F_DUPFD || command == F_SETFL || command == F_SETFD) {
        va_list args; va_start(args, command); argument = va_arg(args, int); va_end(args);
    }
    return do_fcntl(fd, command, argument);
}

int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request, struct timespec *remaining)
{
    if (flags & ~TIMER_ABSTIME) return EINVAL;
    if (clock != CLOCK_MONOTONIC && !(clock == CLOCK_REALTIME && !flags)) return ENOTSUP;
    if (!flags) return nanosleep(request, remaining) ? errno : 0;
    struct timespec target;
    int rc = nos_user_copy(&target, request, sizeof(target));
    if (rc) return -rc;
    if (target.tv_sec < 0 || target.tv_nsec < 0 || target.tv_nsec >= 1000000000L) return EINVAL;
    struct nos_time_value now;
    rc = nos_clock_snapshot(&now);
    if (rc) return -rc;
    if (target.tv_sec < now.seconds ||
        (target.tv_sec == now.seconds && (uint64_t)target.tv_nsec <= now.nanoseconds)) return 0;
    uint64_t seconds = (uint64_t)target.tv_sec - (uint64_t)now.seconds;
    int64_t nanoseconds = (int64_t)target.tv_nsec - now.nanoseconds;
    if (nanoseconds < 0) { seconds--; nanoseconds += 1000000000L; }
    if (seconds > (UINT64_MAX - 100) / 100) return EOVERFLOW;
    uint64_t ticks = seconds * 100 + (uint64_t)(nanoseconds + 9999999) / 10000000;
    rc = nos_sleep_ticks64(ticks, NULL);
    return rc < 0 ? -rc : 0;
}
