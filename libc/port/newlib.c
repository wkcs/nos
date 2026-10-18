/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
#include "user.h"
#include "poll_abi.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <reent.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <poll.h>
#include <sys/select.h>
#include <unistd.h>

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
int nos_newlib_errno(int code)
{
    switch (code) {
    case NOS_EDEADLK: return EDEADLK;
    case NOS_ENOLCK: return ENOLCK;
    case NOS_ENAMETOOLONG: return ENAMETOOLONG;
    case NOS_ENOSYS: return ENOSYS;
    case NOS_ENOTEMPTY: return ENOTEMPTY;
    case 40: return ELOOP;
    case NOS_EOVERFLOW: return EOVERFLOW;
    case NOS_ENOTSUP: return ENOTSUP;
    case NOS_ETIMEDOUT: return ETIMEDOUT;
    case 125: return ECANCELED;
    case 130: return EOWNERDEAD;
    case 131: return ENOTRECOVERABLE;
    default: return code;
    }
}
static intptr_t result(intptr_t rc)
{ if (rc < 0) { errno = nos_newlib_errno((int)-rc); return -1; } return rc; }
_Static_assert(sizeof(struct pollfd) == sizeof(struct nos_pollfd), "Newlib poll ABI");
int poll(struct pollfd *descriptors, nfds_t count, int timeout)
{ return (int)result(nos_poll((struct nos_pollfd *)descriptors, count, timeout)); }
int ppoll(struct pollfd *descriptors, nfds_t count, const struct timespec *timeout, const sigset_t *mask)
{
    if (mask) return (int)result(-NOS_ENOTSUP);
    if (!timeout) return poll(descriptors, count, -1);
    struct timespec value;
    int rc = nos_user_copy(&value, timeout, sizeof(value));
    if (rc) return (int)result(rc);
    if (value.tv_sec < 0 || value.tv_nsec < 0 || value.tv_nsec >= 1000000000L) return (int)result(-NOS_EINVAL);
    if ((uint64_t)value.tv_sec > (UINT64_MAX - 1000) / 1000) return (int)result(-NOS_EOVERFLOW);
    uint64_t milliseconds = (uint64_t)value.tv_sec * 1000 + (value.tv_nsec + 999999) / 1000000;
    for (;;) {
        int part = milliseconds > INT32_MAX ? INT32_MAX : (int)milliseconds;
        int status = poll(descriptors, count, part);
        if (status || milliseconds <= INT32_MAX) return status;
        milliseconds -= INT32_MAX;
    }
}
int select(int count, fd_set *restrict read_set, fd_set *restrict write_set,
    fd_set *restrict exception_set, struct timeval *restrict timeout)
{
    if (count < 0 || count > FD_SETSIZE) return (int)result(-NOS_EINVAL);
    struct timeval value = { .tv_sec = -1, .tv_usec = 0 };
    uint64_t started = 0;
    if (timeout) {
        int rc = nos_user_copy(&value, timeout, sizeof(value));
        if (rc) return (int)result(rc);
        rc = nos_user_check(timeout, sizeof(value), NOS_PROT_WRITE);
        if (rc) return (int)result(rc);
        if (value.tv_sec < 0 || value.tv_usec < 0 || value.tv_usec >= 1000000) return (int)result(-NOS_EINVAL);
        rc = (int)nos_syscall(NOS_SYS_TICKS64, (uintptr_t)&started, 0, 0);
        if (rc) return (int)result(rc);
    }
    intptr_t status = nos_select(count, (uintptr_t)read_set, (uintptr_t)write_set,
        (uintptr_t)exception_set, value.tv_sec, value.tv_usec * 1000L);
    if (timeout) {
        uint64_t now;
        int rc = (int)nos_syscall(NOS_SYS_TICKS64, (uintptr_t)&now, 0, 0);
        if (rc) return (int)result(rc);
        uint64_t original = (uint64_t)value.tv_sec * 1000000 + value.tv_usec;
        uint64_t ticks = now - started;
        uint64_t left = ticks > original / 10000 ? 0 : original - ticks * 10000;
        value.tv_sec = (time_t)(left / 1000000); value.tv_usec = (suseconds_t)(left % 1000000);
        rc = nos_user_copy(timeout, &value, sizeof(value));
        if (rc) return (int)result(rc);
    }
    return (int)result(status);
}
int pselect(int count, fd_set *restrict read_set, fd_set *restrict write_set,
    fd_set *restrict exception_set, const struct timespec *restrict timeout, const sigset_t *restrict mask)
{
    if (mask) return (int)result(-NOS_ENOTSUP);
    if (count < 0 || count > FD_SETSIZE) return (int)result(-NOS_EINVAL);
    struct timespec value = { .tv_sec = -1, .tv_nsec = 0 };
    if (timeout) {
        int rc = nos_user_copy(&value, timeout, sizeof(value));
        if (rc) return (int)result(rc);
        if (value.tv_sec < 0) return (int)result(-NOS_EINVAL);
    }
    return (int)result(nos_select(count, (uintptr_t)read_set, (uintptr_t)write_set,
        (uintptr_t)exception_set, value.tv_sec, value.tv_nsec));
}
static int open_directory(int directory, const char *path, int flags, unsigned mode)
{
    unsigned supported = O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND | O_NONBLOCK | O_EXCL;
#ifdef O_DIRECTORY
    supported |= O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    supported |= O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    supported |= O_CLOEXEC;
#endif
    if ((unsigned)flags & ~supported) { errno = ENOTSUP; return -1; }
    unsigned f = flags & O_ACCMODE;
    if (flags & O_CREAT) f |= NOS_O_CREAT;
    if (flags & O_TRUNC) f |= NOS_O_TRUNC;
    if (flags & O_APPEND) f |= NOS_O_APPEND;
    if (flags & O_NONBLOCK) f |= NOS_O_NONBLOCK;
#ifdef O_DIRECTORY
    if (flags & O_DIRECTORY) f |= NOS_O_DIRECTORY;
#endif
#ifdef O_NOFOLLOW
    if (flags & O_NOFOLLOW) f |= NOS_O_NOFOLLOW;
#endif
#ifdef O_CLOEXEC
    if (flags & O_CLOEXEC) f |= NOS_O_CLOEXEC;
#endif
    if (flags & O_EXCL) f |= NOS_O_EXCL;
    return (int)result(nos_openat_mode(directory, path, f, mode));
}
int _open(const char *path, int flags, ...)
{
    unsigned mode = 0666;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, unsigned); va_end(args); }
    return open_directory(NOS_AT_FDCWD, path, flags, mode);
}
int _close(int fd) { return (int)result(nos_close(fd)); }
_ssize_t _read(int fd, void *buf, size_t n) { return (_ssize_t)result(nos_posix_read(fd, buf, n)); }
_ssize_t _write(int fd, const void *buf, size_t n) { return (_ssize_t)result(nos_posix_write(fd, buf, n)); }
off_t _lseek(int fd, off_t offset, int whence) { return (off_t)result(nos_seek(fd, offset, whence)); }
static int stat_value(const struct nos_stat64 *info, struct stat *st)
{
    struct stat value;
    u_memset(&value, 0, sizeof(value));
    value.st_ino = info->ino; value.st_mode = info->mode; value.st_size = (off_t)info->size;
    value.st_blocks = info->blocks; value.st_blksize = 512; value.st_nlink = info->nlink;
    value.st_uid = info->uid; value.st_gid = info->gid;
    value.st_atime = info->atime_sec; value.st_mtime = info->mtime_sec;
    value.st_ctime = info->ctime_sec;
    if (value.st_size < 0 || (uint64_t)value.st_size != info->size ||
        (uint64_t)value.st_ino != info->ino || (uint64_t)value.st_blocks != info->blocks ||
        (uint32_t)value.st_nlink != info->nlink || (uint32_t)value.st_uid != info->uid ||
        (uint32_t)value.st_gid != info->gid || (int64_t)value.st_atime != info->atime_sec ||
        (int64_t)value.st_mtime != info->mtime_sec || (int64_t)value.st_ctime != info->ctime_sec)
        return (int)result(-NOS_EOVERFLOW);
    return (int)result(nos_user_copy(st, &value, sizeof(value)));
}
int _fstat(int fd, struct stat *st)
{
    struct nos_stat64 info;
    intptr_t rc = nos_stat64_fd(fd, &info);
    return rc ? (int)result(rc) : stat_value(&info, st);
}
int _isatty(int fd)
{
    struct nos_stat info;
    intptr_t rc = nos_stat_fd(fd, &info);
    if (rc < 0) { errno = nos_newlib_errno((int)-rc); return 0; }
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
    (void)zone;
    struct nos_time_value snapshot;
    int rc = nos_clock_read(0, &snapshot);
    if (rc) return (int)result(rc);
    struct timeval value = { .tv_sec = (time_t)snapshot.seconds,
        .tv_usec = (suseconds_t)(snapshot.nanoseconds / 1000) };
    if ((int64_t)value.tv_sec != snapshot.seconds) return (int)result(-NOS_EOVERFLOW);
    return (int)result(nos_user_copy(tv, &value, sizeof(value)));
}
int _unlink(const char *path) { return (int)result(nos_unlink(path)); }
int _rename(const char *old_path, const char *new_path)
{ return (int)result(nos_rename(old_path, new_path)); }
int _mkdir(const char *path, mode_t mode)
{ return (int)result(nos_mkdirat(NOS_AT_FDCWD, path, mode)); }
int rmdir(const char *path) { return (int)result(nos_rmdir(path)); }
int dup(int fd) { return (int)result(nos_dup(fd)); }
int dup2(int fd, int target) { return (int)result(nos_dup2(fd, target)); }
int pipe(int descriptors[2]) { return (int)result(nos_pipe(descriptors)); }

static int path_stat(int directory, const char *path, int flags, struct stat *st)
{
    struct nos_stat64 info;
    int rc = nos_stat_at(directory, path, (unsigned)flags, &info);
    if (rc) return (int)result(rc);
    return stat_value(&info, st);
}
int _stat(const char *path, struct stat *st) { return path_stat(NOS_AT_FDCWD, path, 0, st); }
int lstat(const char *path, struct stat *st)
{ return path_stat(NOS_AT_FDCWD, path, NOS_AT_SYMLINK_NOFOLLOW, st); }
int fstatat(int directory, const char *path, struct stat *st, int flags)
{ return path_stat(directory, path, flags, st); }
int openat(int directory, const char *path, int flags, ...)
{
    unsigned mode = 0666;
    if (flags & O_CREAT) { va_list args; va_start(args, flags); mode = va_arg(args, unsigned); va_end(args); }
    return open_directory(directory, path, flags, mode);
}
int mkdirat(int directory, const char *path, mode_t mode)
{ return (int)result(nos_mkdirat(directory, path, mode)); }
int unlinkat(int directory, const char *path, int flags)
{ return (int)result(nos_unlinkat(directory, path, (unsigned)flags)); }
int renameat(int old_directory, const char *old_path, int new_directory, const char *new_path)
{ return (int)result(nos_renameat(old_directory, old_path, new_directory, new_path)); }
int ftruncate(int fd, off_t length) { return (int)result(nos_ftruncate(fd, length)); }
int truncate(const char *path, off_t length) { return (int)result(nos_truncate(path, length)); }
mode_t umask(mode_t mask) { return (mode_t)result(nos_umask(mask)); }
int fchmod(int fd, mode_t mode) { return (int)result(nos_fchmod(fd, mode)); }
int chmod(const char *path, mode_t mode) { return (int)result(nos_chmod_at(NOS_AT_FDCWD, path, mode)); }
int fchmodat(int directory, const char *path, mode_t mode, int flags)
{ return flags ? (int)result(-NOS_ENOTSUP) : (int)result(nos_chmod_at(directory, path, mode)); }
int symlinkat(const char *target, int directory, const char *path)
{ return (int)result(nos_symlink_at(target, directory, path)); }
int symlink(const char *target, const char *path) { return symlinkat(target, NOS_AT_FDCWD, path); }
int linkat(int old_directory, const char *old_path, int new_directory, const char *new_path, int flags)
{ return flags ? (int)result(-NOS_ENOTSUP) : (int)result(nos_link_at(old_directory, old_path, new_directory, new_path)); }
int _link(const char *old_path, const char *new_path)
{ return linkat(NOS_AT_FDCWD, old_path, NOS_AT_FDCWD, new_path, 0); }
ssize_t pread(int fd, void *buffer, size_t count, off_t offset)
{ return (ssize_t)result(nos_pread(fd, buffer, count, offset)); }
ssize_t pwrite(int fd, const void *buffer, size_t count, off_t offset)
{ return (ssize_t)result(nos_pwrite(fd, buffer, count, offset)); }
int chdir(const char *path) { return (int)result(nos_chdir(path)); }
int fchdir(int fd) { return (int)result(nos_fchdir(fd)); }
char *getcwd(char *buffer, size_t size)
{
    int allocated = !buffer;
    if (allocated) {
        if (!size) size = NOS_PATH_MAX;
        buffer = malloc(size);
        if (!buffer) return NULL;
    }
    intptr_t rc = nos_getcwd(buffer, size);
    if (rc < 0) {
        if (allocated) free(buffer);
        result(rc); return NULL;
    }
    return buffer;
}
ssize_t readlinkat(int directory, const char *path, char *buffer, size_t size)
{ return (ssize_t)result(nos_readlink_at(directory, path, buffer, size)); }
ssize_t readlink(const char *path, char *buffer, size_t size)
{ return readlinkat(NOS_AT_FDCWD, path, buffer, size); }

struct nos_directory {
    int fd, lock;
    size_t used, cursor;
    long position;
    unsigned char bytes[NOS_DIRENT_RECORD_MAX];
    struct dirent entry;
};
static void directory_lock(DIR *directory)
{
    while (__atomic_exchange_n(&directory->lock, 1, __ATOMIC_ACQUIRE))
        nos_syscall(NOS_SYS_FUTEX_WAIT, (uintptr_t)&directory->lock, 1, 0);
}
static void directory_unlock(DIR *directory)
{
    __atomic_store_n(&directory->lock, 0, __ATOMIC_RELEASE);
    nos_syscall(NOS_SYS_FUTEX_WAKE, (uintptr_t)&directory->lock, 1, 0);
}
DIR *fdopendir(int fd)
{
    struct nos_stat64 st;
    int rc = nos_stat64_fd(fd, &st);
    if (rc) { result(rc); return NULL; }
    if ((st.mode & S_IFMT) != S_IFDIR) { errno = ENOTDIR; return NULL; }
    int flags = nos_getflags(fd);
    if (flags < 0) { result(flags); return NULL; }
    if ((flags & 3) == NOS_O_WRONLY) { errno = EBADF; return NULL; }
    DIR *directory = calloc(1, sizeof(*directory));
    if (directory) directory->fd = fd;
    return directory;
}
DIR *opendir(const char *path)
{
    int fd = nos_open(path, NOS_O_RDONLY | NOS_O_CLOEXEC);
    if (fd < 0) { result(fd); return NULL; }
    DIR *directory = fdopendir(fd);
    if (!directory) { int saved = errno; nos_close(fd); errno = saved; }
    return directory;
}
static struct dirent *directory_read(DIR *directory)
{
    if (directory->cursor == directory->used) {
        intptr_t rc = nos_getdents(directory->fd, directory->bytes, sizeof(directory->bytes));
        if (rc <= 0) { if (rc) result(rc); return NULL; }
        directory->cursor = 0; directory->used = (size_t)rc;
    }
    unsigned char *record = directory->bytes + directory->cursor;
    uint16_t length;
    uint64_t inode;
    int64_t position;
    if (directory->used - directory->cursor < 24) { errno = EIO; return NULL; }
    u_memcpy(&length, record + 16, 2);
    if (length < 24 || length > directory->used - directory->cursor) { errno = EIO; return NULL; }
    u_memcpy(&inode, record, 8); u_memcpy(&position, record + 8, 8);
    if ((uint64_t)(ino_t)inode != inode || (int64_t)(long)position != position) {
        errno = EOVERFLOW; return NULL;
    }
    size_t namesize = 0;
    while (19 + namesize < length && record[19 + namesize]) namesize++;
    if (namesize >= sizeof(directory->entry.d_name) || 19 + namesize == length) { errno = EIO; return NULL; }
    u_memset(&directory->entry, 0, sizeof(directory->entry));
    directory->entry.d_ino = (ino_t)inode; directory->entry.d_off = (off_t)position;
    directory->entry.d_reclen = sizeof(directory->entry); directory->entry.d_type = record[18];
    u_memcpy(directory->entry.d_name, record + 19, namesize + 1);
    directory->cursor += length; directory->position = (long)position;
    return &directory->entry;
}
struct dirent *readdir(DIR *directory)
{
    if (!directory) { errno = EBADF; return NULL; }
    directory_lock(directory);
    struct dirent *entry = directory_read(directory);
    directory_unlock(directory);
    return entry;
}
int readdir_r(DIR *restrict directory, struct dirent *restrict entry, struct dirent **restrict output)
{
    if (!directory || !entry || !output) return EINVAL;
    directory_lock(directory);
    int saved = errno;
    errno = 0;
    struct dirent *next = directory_read(directory);
    int error = errno;
    if (next) { *entry = *next; *output = entry; } else *output = NULL;
    errno = saved;
    directory_unlock(directory);
    return error;
}
int dirfd(DIR *directory)
{ if (!directory) { errno = EBADF; return -1; } return directory->fd; }
long telldir(DIR *directory)
{
    if (!directory) { errno = EBADF; return -1; }
    directory_lock(directory);
    long position = directory->position;
    directory_unlock(directory);
    return position;
}
void seekdir(DIR *directory, long location)
{
    if (!directory) { errno = EBADF; return; }
    directory_lock(directory);
    intptr_t rc = nos_seek(directory->fd, location, 0);
    if (rc < 0) result(rc);
    else { directory->used = directory->cursor = 0; directory->position = location; }
    directory_unlock(directory);
}
void rewinddir(DIR *directory) { seekdir(directory, 0); }
int closedir(DIR *directory)
{
    if (!directory) { errno = EBADF; return -1; }
    int rc = nos_close(directory->fd);
    if (rc) return (int)result(rc);
    free(directory); return 0;
}
int alphasort(const struct dirent **left, const struct dirent **right)
{ return strcoll((*left)->d_name, (*right)->d_name); }
static int scan_directory(DIR *directory, struct dirent ***output,
    int (*select_entry)(const struct dirent *),
    int (*compare)(const struct dirent **, const struct dirent **))
{
    if (!directory) return -1;
    if (!output) { closedir(directory); errno = EINVAL; return -1; }
    int saved = errno, error = 0;
    size_t count = 0, capacity = 0;
    struct dirent **entries = NULL;
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(directory);
        if (!entry) { error = errno; break; }
        if (select_entry && !select_entry(entry)) continue;
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 4;
            if (next < capacity || next > INT32_MAX || next > SIZE_MAX / sizeof(*entries)) {
                error = EOVERFLOW; break;
            }
            struct dirent **grown = realloc(entries, next * sizeof(*entries));
            if (!grown) { error = errno; break; }
            entries = grown; capacity = next;
        }
        entries[count] = malloc(sizeof(*entry));
        if (!entries[count]) { error = errno; break; }
        *entries[count++] = *entry;
    }
    if (closedir(directory) && !error) error = errno;
    if (error) {
        while (count) free(entries[--count]);
        free(entries); errno = error; return -1;
    }
    if (compare) {
        for (size_t i = 1; i < count; i++) {
            struct dirent *entry = entries[i];
            size_t position = i;
            while (position && compare((const struct dirent **)&entries[position - 1],
                    (const struct dirent **)&entry) > 0) {
                entries[position] = entries[position - 1]; position--;
            }
            entries[position] = entry;
        }
    }
    *output = entries; errno = saved; return (int)count;
}
int scandir(const char *path, struct dirent ***output,
    int (*select_entry)(const struct dirent *),
    int (*compare)(const struct dirent **, const struct dirent **))
{ return scan_directory(opendir(path), output, select_entry, compare); }
int scandirat(int directory, const char *path, struct dirent ***output,
    int (*select_entry)(const struct dirent *),
    int (*compare)(const struct dirent **, const struct dirent **))
{
    int fd = nos_openat(directory, path, NOS_O_RDONLY | NOS_O_DIRECTORY | NOS_O_CLOEXEC);
    if (fd < 0) return (int)result(fd);
    DIR *stream = fdopendir(fd);
    if (!stream) { int saved = errno; nos_close(fd); errno = saved; return -1; }
    return scan_directory(stream, output, select_entry, compare);
}

#include <time.h>
int clock_gettime(clockid_t clock, struct timespec *time)
{
    if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME) { errno = EINVAL; return -1; }
    struct nos_time_value snapshot;
    int rc = nos_clock_read(clock == CLOCK_REALTIME ? 0u : 1u, &snapshot);
    if (rc) return (int)result(rc);
    struct timespec value = { .tv_sec = (time_t)snapshot.seconds, .tv_nsec = snapshot.nanoseconds };
    if ((int64_t)value.tv_sec != snapshot.seconds) return (int)result(-NOS_EOVERFLOW);
    return (int)result(nos_user_copy(time, &value, sizeof(value)));
}
int clock_getres(clockid_t clock, struct timespec *time)
{
    if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME) return (int)result(-NOS_EINVAL);
    struct nos_time_value snapshot;
    int rc = nos_clock_read(clock == CLOCK_REALTIME ? 0u : 1u, &snapshot);
    if (rc) return (int)result(rc);
    struct timespec value = { clock == CLOCK_REALTIME ? 1 : 0, clock == CLOCK_REALTIME ? 0 : 10000000 };
    return time ? (int)result(nos_user_copy(time, &value, sizeof(value))) : 0;
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
static int newlib_record_lock(int fd, int command, struct flock *pointer)
{
    struct flock input;
    int rc;
    if (command == F_GETLK && (rc = nos_user_check(pointer, sizeof(input), NOS_PROT_WRITE))) return (int)result(rc);
    rc = nos_user_copy(&input, pointer, sizeof(input));
    if (rc) return (int)result(rc);
    if (input.l_type != F_RDLCK && input.l_type != F_WRLCK && input.l_type != F_UNLCK) {
        rc = -NOS_EINVAL; return (int)result(rc);
    }
    if (input.l_whence != SEEK_SET && input.l_whence != SEEK_CUR && input.l_whence != SEEK_END) {
        rc = -NOS_EINVAL; return (int)result(rc);
    }
    struct nos_record_lock lock = { .start = input.l_start, .length = input.l_len,
        .type = input.l_type == F_RDLCK ? NOS_LOCK_READ : input.l_type == F_WRLCK ? NOS_LOCK_WRITE : NOS_LOCK_UNLOCK,
        .whence = input.l_whence == SEEK_SET ? 0 : input.l_whence == SEEK_CUR ? 1 : 2 };
    unsigned operation = command == F_GETLK ? NOS_LOCK_GET : command == F_SETLK ? NOS_LOCK_SET : NOS_LOCK_SET_WAIT;
    rc = nos_record_lock(fd, operation, &lock);
    if (rc) return (int)result(rc);
    if (operation != NOS_LOCK_GET) return 0;
    short type = lock.type == NOS_LOCK_READ ? F_RDLCK : lock.type == NOS_LOCK_WRITE ? F_WRLCK : F_UNLCK;
    if (lock.type == NOS_LOCK_UNLOCK) {
        rc = nos_user_copy(&pointer->l_type, &type, sizeof(type)); return (int)result(rc);
    }
    input.l_type = type; input.l_whence = SEEK_SET;
    input.l_start = lock.start; input.l_len = lock.length; input.l_pid = lock.pid;
    if ((int64_t)input.l_start != lock.start || (int64_t)input.l_len != lock.length ||
        (int32_t)input.l_pid != lock.pid) { rc = -NOS_EOVERFLOW; return (int)result(rc); }
    rc = nos_user_copy(pointer, &input, sizeof(input)); return (int)result(rc);
}
static int do_fcntl(int fd, int command, int argument)
{
    if (command == F_DUPFD) return (int)result(nos_dup_min(fd, (unsigned)argument));
#ifdef F_DUPFD_CLOEXEC
    if (command == F_DUPFD_CLOEXEC) return (int)result(nos_dup_cloexec(fd, (unsigned)argument));
#endif
    int flags = nos_getflags(fd);
    if (flags < 0) return (int)result(flags);
    if (command == F_GETFL) return (flags & 3) |
        (flags & NOS_O_APPEND ? O_APPEND : 0) | (flags & NOS_O_NONBLOCK ? O_NONBLOCK : 0);
    if (command == F_SETFL) {
        if ((unsigned)argument & ~(O_ACCMODE | O_APPEND | O_NONBLOCK)) { errno = ENOTSUP; return -1; }
        return (int)result(nos_setflags(fd, (argument & O_APPEND ? NOS_O_APPEND : 0) |
            (argument & O_NONBLOCK ? NOS_O_NONBLOCK : 0)));
    }
    if (command == F_GETFD) return (int)result(nos_getfd(fd));
    if (command == F_SETFD) {
        if ((unsigned)argument & ~FD_CLOEXEC) return (int)result(-NOS_EINVAL);
        return (int)result(nos_setfd(fd, argument & FD_CLOEXEC ? 1 : 0));
    }
    errno = ENOTSUP; return -1;
}
int _fcntl(int fd, int command, ...)
{
    if (command == F_GETLK || command == F_SETLK || command == F_SETLKW) {
        va_list args; va_start(args, command); struct flock *lock = va_arg(args, struct flock *); va_end(args);
        return newlib_record_lock(fd, command, lock);
    }
    int argument = 0;
    if (command == F_DUPFD || command == F_SETFL || command == F_SETFD
#ifdef F_DUPFD_CLOEXEC
        || command == F_DUPFD_CLOEXEC
#endif
        ) {
        va_list args; va_start(args, command); argument = va_arg(args, int); va_end(args);
    }
    return do_fcntl(fd, command, argument);
}
int fcntl(int fd, int command, ...)
{
    if (command == F_GETLK || command == F_SETLK || command == F_SETLKW) {
        va_list args; va_start(args, command); struct flock *lock = va_arg(args, struct flock *); va_end(args);
        return newlib_record_lock(fd, command, lock);
    }
    int argument = 0;
    if (command == F_DUPFD || command == F_SETFL || command == F_SETFD
#ifdef F_DUPFD_CLOEXEC
        || command == F_DUPFD_CLOEXEC
#endif
        ) {
        va_list args; va_start(args, command); argument = va_arg(args, int); va_end(args);
    }
    return do_fcntl(fd, command, argument);
}

int clock_nanosleep(clockid_t clock, int flags, const struct timespec *request, struct timespec *remaining)
{
    if (flags & ~TIMER_ABSTIME) return EINVAL;
    if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME) return EINVAL;
    if (!flags) return nanosleep(request, remaining) ? errno : 0;
    struct timespec target;
    int rc = nos_user_copy(&target, request, sizeof(target));
    if (rc) return nos_newlib_errno(-rc);
    if (target.tv_sec < 0 || target.tv_nsec < 0 || target.tv_nsec >= 1000000000L) return EINVAL;
    struct nos_time_value now;
    rc = nos_clock_read(clock == CLOCK_REALTIME ? 0u : 1u, &now);
    if (rc) return nos_newlib_errno(-rc);
    if (target.tv_sec < now.seconds ||
        (target.tv_sec == now.seconds && (uint64_t)target.tv_nsec <= now.nanoseconds)) return 0;
    uint64_t seconds = (uint64_t)target.tv_sec - (uint64_t)now.seconds;
    int64_t nanoseconds = (int64_t)target.tv_nsec - now.nanoseconds;
    if (nanoseconds < 0) { seconds--; nanoseconds += 1000000000L; }
    if (seconds > (UINT64_MAX - 100) / 100) return EOVERFLOW;
    uint64_t ticks = seconds * 100 + (uint64_t)(nanoseconds + 9999999) / 10000000;
    rc = nos_sleep_ticks64(ticks, NULL);
    return rc < 0 ? nos_newlib_errno(-rc) : 0;
}
