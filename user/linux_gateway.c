/* Copyright (C) 2026 NOS contributors */
/* A Linux syscall upcall executes as an ordinary NOS user task. Files and
 * descriptors stay in the VFS service; this code never dereferences kernel
 * data or relies on the application's libc/TLS being initialized. */
#include "linux.h"
#include "user.h"
#include "poll_abi.h"
#include <errno.h>
#include <limits.h>
#include <asm/stat.h>
#include <linux/fcntl.h>
#include <linux/stat.h>
#include <sys/syscall.h>

#define LINUX_AT_FDCWD (-100)
#define LINUX_AT_SYMLINK_NOFOLLOW 0x100
#define LINUX_AT_REMOVEDIR 0x200
#define LINUX_AT_EMPTY_PATH 0x1000
#define LINUX_AT_NO_AUTOMOUNT 0x800
#define LINUX_MAP_SHARED 1u
#define LINUX_MAP_PRIVATE 2u
#define LINUX_MAP_FIXED 0x10u
#define LINUX_MAP_ANONYMOUS 0x20u
#define LINUX_MAP_DENYWRITE 0x800u
#define LINUX_MAP_EXECUTABLE 0x1000u
#define LINUX_MAP_FIXED_NOREPLACE 0x100000u

static int pointer_error(intptr_t value) { return nos_pointer_is_error(value); }
static int fd_valid(uintptr_t value)
{ return value == (uintptr_t)(intptr_t)(int)value; }
static int open_flags(unsigned long flags, unsigned *result)
{
    unsigned long supported = O_ACCMODE | O_CREAT | O_EXCL | O_TRUNC | O_APPEND |
        O_NONBLOCK | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW;
#ifdef O_LARGEFILE
    supported |= O_LARGEFILE;
#endif
    if (flags & ~supported || (flags & O_ACCMODE) == O_ACCMODE) return -ENOTSUP;
    unsigned f = (flags & O_ACCMODE) == O_WRONLY ? NOS_O_WRONLY :
                 (flags & O_ACCMODE) == O_RDWR ? NOS_O_RDWR : NOS_O_RDONLY;
    if (flags & O_CREAT) f |= NOS_O_CREAT;
    if (flags & O_EXCL) f |= NOS_O_EXCL;
    if (flags & O_TRUNC) f |= NOS_O_TRUNC;
    if (flags & O_APPEND) f |= NOS_O_APPEND;
    if (flags & O_NONBLOCK) f |= NOS_O_NONBLOCK;
    if (flags & O_DIRECTORY) f |= NOS_O_DIRECTORY;
    /* CLOEXEC and NOFOLLOW are passed through native flag bits by the
     * wrapper once the VFS advertises those semantics. */
#ifdef NOS_O_CLOEXEC
    if (flags & O_CLOEXEC) f |= NOS_O_CLOEXEC;
#else
    if (flags & O_CLOEXEC) return -ENOTSUP;
#endif
#ifdef NOS_O_NOFOLLOW
    if (flags & O_NOFOLLOW) f |= NOS_O_NOFOLLOW;
#else
    if (flags & O_NOFOLLOW) return -ENOTSUP;
#endif
    *result = f;
    return 0;
}
static intptr_t gateway_openat(int dirfd, uintptr_t path, unsigned long flags, unsigned mode)
{
    unsigned native;
    int rc = open_flags(flags, &native);
    return rc ? rc : nos_openat_mode(dirfd, (const char *)path, native, mode);
}
static int native_stat(int dirfd, uintptr_t path, int fd, unsigned flags, struct nos_stat64 *st)
{
    /* NOS has no automount facility, so suppressing automount changes no
     * lookup behavior. Other AT flags are checked by the VFS wrapper. */
    flags &= ~LINUX_AT_NO_AUTOMOUNT;
    return fd < 0 ? nos_stat_at(dirfd, (const char *)path, flags, st) : nos_stat64_fd(fd, st);
}
static intptr_t gateway_stat(int dirfd, uintptr_t path, int fd, uintptr_t output, unsigned flags, int wide)
{
    struct nos_stat64 st;
    int rc = native_stat(dirfd, path, fd, flags, &st);
    if (rc) return rc;
#if defined(NOS_ARM)
    if (wide) {
        struct stat64 value;
        u_memset(&value, 0, sizeof(value));
        value.st_ino = st.ino; value.__st_ino = (unsigned long)st.ino;
        value.st_mode = st.mode; value.st_nlink = st.nlink;
        value.st_uid = st.uid; value.st_gid = st.gid;
        value.st_size = st.size; value.st_blocks = st.blocks; value.st_blksize = NOS_PAGE_SIZE;
        value.st_atime = st.atime_sec; value.st_mtime = st.mtime_sec; value.st_ctime = st.ctime_sec;
        return nos_user_copy((void *)output, &value, sizeof(value));
    }
    if (st.ino > ULONG_MAX || st.size > LONG_MAX || st.blocks > ULONG_MAX) return -EOVERFLOW;
#else
    (void)wide;
#endif
    struct stat value;
    u_memset(&value, 0, sizeof(value));
    value.st_ino = st.ino; value.st_mode = st.mode; value.st_nlink = st.nlink;
    value.st_uid = st.uid; value.st_gid = st.gid;
    value.st_size = st.size; value.st_blocks = st.blocks; value.st_blksize = NOS_PAGE_SIZE;
    value.st_atime = st.atime_sec; value.st_mtime = st.mtime_sec; value.st_ctime = st.ctime_sec;
    return nos_user_copy((void *)output, &value, sizeof(value));
}
static intptr_t gateway_statx(int dirfd, uintptr_t path, unsigned flags, uintptr_t output)
{
    struct nos_stat64 st;
    int rc = native_stat(dirfd, path, -1, flags, &st);
    if (rc) return rc;
    struct statx value;
    u_memset(&value, 0, sizeof(value));
    value.stx_mask = STATX_TYPE | STATX_MODE | STATX_NLINK | STATX_UID | STATX_GID |
        STATX_INO | STATX_SIZE | STATX_BLOCKS | STATX_ATIME | STATX_MTIME | STATX_CTIME;
    value.stx_blksize = NOS_PAGE_SIZE; value.stx_ino = st.ino;
    value.stx_mode = st.mode; value.stx_nlink = st.nlink;
    value.stx_uid = st.uid; value.stx_gid = st.gid;
    value.stx_size = st.size; value.stx_blocks = st.blocks;
    value.stx_atime.tv_sec = st.atime_sec; value.stx_mtime.tv_sec = st.mtime_sec;
    value.stx_ctime.tv_sec = st.ctime_sec;
    return nos_user_copy((void *)output, &value, sizeof(value));
}
struct linux_iovec { uintptr_t base; size_t length; };
/* The only PIPE_BUF-sized gateway scratch frame. Keep this out of line:
 * runtime write_mode binds the buffer directly and allocates no second copy. */
static __attribute__((noinline)) intptr_t vector_write_atomic(int fd,
    const struct linux_iovec *vectors, size_t count, size_t total)
{
    unsigned char gathered[NOS_PIPE_BUF];
    size_t done = 0;
    for (size_t i = 0; i < count; i++) {
        int rc = nos_user_copy(gathered + done, (const void *)vectors[i].base, vectors[i].length);
        if (rc) return rc;
        done += vectors[i].length;
    }
    return nos_posix_write(fd, gathered, total);
}
static intptr_t vector_io_snapshot(int fd, const struct linux_iovec *vectors, size_t count, int write)
{
    size_t total = 0;
    for (size_t i = 0; i < count; i++) {
        if (vectors[i].length > (size_t)INTPTR_MAX - total) return -EINVAL;
        total += vectors[i].length;
    }
    for (size_t i = 0; i < count; i++) {
        int rc = nos_user_check((const void *)vectors[i].base, vectors[i].length,
            write ? NOS_PROT_READ : NOS_PROT_WRITE);
        if (rc) return rc;
    }
    if (!total) return write ? nos_write(fd, NULL, 0) : nos_read(fd, NULL, 0);
    if (write && total <= NOS_PIPE_BUF) return vector_write_atomic(fd, vectors, count, total);
    intptr_t done = 0;
    for (size_t i = 0; i < count; i++) {
        if (!vectors[i].length) continue;
        /* Only a read with a completed prefix requests DONTWAIT. A blocking
         * writev larger than PIPE_BUF may interleave, but must keep waiting. */
        intptr_t n = write ? nos_posix_write_large(fd, (const void *)vectors[i].base, vectors[i].length) :
            (done ? nos_read_nowait(fd, (void *)vectors[i].base, vectors[i].length) :
                nos_posix_read(fd, (void *)vectors[i].base, vectors[i].length));
        if (n < 0) return done ? done : n;
        done += n;
        if ((size_t)n < vectors[i].length) break;
    }
    return done;
}
static intptr_t vector_io(int fd, uintptr_t pointer, uintptr_t count, int write)
{
    /* Linux UIO_MAXIOV (and glibc _SC_IOV_MAX) is 1024. Snapshot before any
     * I/O; neither a 16KiB array nor a mutable application iovec goes on the
     * 8KiB gateway stack. Small calls retain the allocation-free fast path. */
    struct linux_iovec local[16], *vectors = local;
    if (count > 1024) return -EINVAL;
    size_t bytes = count * sizeof(*vectors), mapped = 0;
    if (count > sizeof(local) / sizeof(local[0])) {
        mapped = (bytes + NOS_PAGE_SIZE - 1) & ~(size_t)(NOS_PAGE_SIZE - 1);
        intptr_t address = nos_syscall(NOS_SYS_MMAP, NOS_PROT_READ | NOS_PROT_WRITE, mapped, 0);
        if (pointer_error(address)) return address;
        vectors = (void *)(uintptr_t)address;
    }
    int rc = nos_user_copy(vectors, (const void *)pointer, bytes);
    intptr_t result = rc ? rc : vector_io_snapshot(fd, vectors, count, write);
    /* GATEWAY signals are delivered after LINUX_RETURN, so an interrupted
     * RPC reaches this cleanup before cancellation/SA_RESTART is delivered. */
    if (mapped) (void)nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)vectors, mapped, 0);
    return result;
}
static intptr_t gateway_getrandom(uintptr_t buffer, size_t count, unsigned flags)
{
    if (flags & ~1u || count > INTPTR_MAX) return -EINVAL;
    int rc = nos_user_check((void *)buffer, count, NOS_PROT_WRITE);
    if (rc) return rc;
    size_t done = 0;
    while (done < count) {
        struct nos_message message = { .op = NOS_VFS_GETRANDOM, .arg = flags,
            .len = count - done < NOS_MSG_DATA ? count - done : NOS_MSG_DATA };
        size_t requested = message.len;
        intptr_t n = nos_rpc(&message);
        if (n < 0) return done ? (intptr_t)done : n;
        if (!n || (size_t)n > requested || message.len < (size_t)n) return done ? (intptr_t)done : -EIO;
        rc = nos_user_copy((void *)(buffer + done), message.data, (size_t)n);
        if (rc) return done ? (intptr_t)done : rc;
        done += (size_t)n;
    }
    return (intptr_t)done;
}
static intptr_t gateway_mmap(uintptr_t address, size_t length, unsigned prot, unsigned flags, int fd, uint64_t offset)
{
    if (!length || length > SIZE_MAX - NOS_PAGE_SIZE + 1 || prot & ~7u || (prot & 6) == 6 ||
        flags & ~(LINUX_MAP_SHARED | LINUX_MAP_PRIVATE | LINUX_MAP_FIXED | LINUX_MAP_ANONYMOUS |
                  LINUX_MAP_DENYWRITE | LINUX_MAP_EXECUTABLE | LINUX_MAP_FIXED_NOREPLACE) ||
        ((flags & 3) != LINUX_MAP_PRIVATE && (flags & 3) != LINUX_MAP_SHARED) ||
        offset & (NOS_PAGE_SIZE - 1)) return -EINVAL;
    if (offset > UINTPTR_MAX) return -EOVERFLOW;
    /* These MMUs expose readable write/execute mappings. Keep the native
     * permission ABI strict while normalizing the Linux personality. */
    if (prot & (NOS_PROT_WRITE | NOS_PROT_EXEC)) prot |= NOS_PROT_READ;
    struct nos_linux_map request = { address, length, prot, flags, (uintptr_t)fd, (uintptr_t)offset };
    if (flags & LINUX_MAP_ANONYMOUS)
        return nos_syscall(NOS_SYS_LINUX_MAP, (uintptr_t)&request, 0, 0);
    if (flags & LINUX_MAP_SHARED) {
        if (!fd_valid((uintptr_t)(intptr_t)fd)) return -EBADF;
        /* The VFS validates the descriptor and returns a one-use mapping
         * lease. The kernel enforces its object, process and permissions;
         * no physical address or filesystem business crosses this ABI. */
        struct nos_message message = { .op = NOS_VFS_SHARED_FILE, .arg = fd, .value = prot };
        intptr_t lease = nos_rpc(&message);
        if (lease < 0) return lease;
        if (!lease || message.len != sizeof(uint64_t)) return -EIO;
        struct nos_shared_request shared = { .handle = (uintptr_t)lease, .address = address,
            .length = length, .offset = (uintptr_t)offset, .prot = prot, .flags = flags };
        return nos_syscall(NOS_SYS_SHARED, NOS_SHARED_MAP, (uintptr_t)&shared, 0);
    }
    struct nos_stat64 st;
    int rc = nos_stat64_fd(fd, &st);
    if (rc) return rc;
    if ((st.mode & 0170000) != 0100000) return -ENODEV;
    /* File content is filled while RW/NX, then permissions are finalized.
     * There is never a writable executable mapping. */
    request.prot = NOS_PROT_READ | NOS_PROT_WRITE;
    intptr_t result = nos_syscall(NOS_SYS_LINUX_MAP, (uintptr_t)&request, 0, 0);
    if (pointer_error(result)) return result;
    size_t bytes = offset >= st.size ? 0 : st.size - offset < length ? (size_t)(st.size - offset) : length;
    size_t done = 0;
    while (done < bytes) {
        intptr_t n = nos_pread(fd, (void *)((uintptr_t)result + done), bytes - done, offset + done);
        if (n <= 0) { rc = n < 0 ? (int)n : -EIO; break; }
        if ((size_t)n > bytes - done) { rc = -EIO; break; }
        done += (size_t)n;
    }
    if (!rc) rc = (int)nos_syscall(NOS_SYS_MPROTECT, (uintptr_t)result, prot, length);
    if (rc) { (void)nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)result, length, 0); return rc; }
    return result;
}
/* Linux flock layouts follow the target UAPI, independently of whether the
 * application reached SYS_fcntl or ARM SYS_fcntl64. */
static intptr_t gateway_record_lock(int fd, unsigned command, uintptr_t pointer)
{
    unsigned operation = command == F_GETLK ? NOS_LOCK_GET :
        command == F_SETLK ? NOS_LOCK_SET : NOS_LOCK_SET_WAIT;
#if defined(NOS_ARM)
    int wide = command == F_GETLK64 || command == F_SETLK64 || command == F_SETLKW64;
    if (wide) operation = command == F_GETLK64 ? NOS_LOCK_GET :
        command == F_SETLK64 ? NOS_LOCK_SET : NOS_LOCK_SET_WAIT;
    size_t bytes = wide ? sizeof(struct flock64) : sizeof(struct flock);
#else
    size_t bytes = sizeof(struct flock);
#endif
    struct nos_record_lock lock = { 0 };
    int rc;
#if defined(NOS_ARM)
    if (wide) {
        struct flock64 input;
        rc = nos_user_copy(&input, (const void *)pointer, sizeof(input));
        if (rc) return rc;
        lock.start = input.l_start; lock.length = input.l_len;
        lock.type = input.l_type; lock.whence = input.l_whence;
    } else
#endif
    {
        struct flock input;
        rc = nos_user_copy(&input, (const void *)pointer, sizeof(input));
        if (rc) return rc;
        lock.start = input.l_start; lock.length = input.l_len;
        lock.type = input.l_type; lock.whence = input.l_whence;
    }
    if (lock.type != F_RDLCK && lock.type != F_WRLCK && lock.type != F_UNLCK) return -EINVAL;
    lock.type = lock.type == F_RDLCK ? NOS_LOCK_READ :
        lock.type == F_WRLCK ? NOS_LOCK_WRITE : NOS_LOCK_UNLOCK;
    if (operation == NOS_LOCK_GET) {
        rc = nos_user_check((void *)pointer, bytes, NOS_PROT_WRITE);
        if (rc) return rc;
    }
    rc = nos_record_lock(fd, operation, &lock);
    if (rc || operation != NOS_LOCK_GET) return rc;
    short type = lock.type == NOS_LOCK_READ ? F_RDLCK :
        lock.type == NOS_LOCK_WRITE ? F_WRLCK : F_UNLCK;
    /* Only l_type is changed for an unlocked result. Linux preserves the
     * application's requested range, whence and ignored input pid. */
    if (lock.type == NOS_LOCK_UNLOCK)
        return nos_user_copy((void *)pointer, &type, sizeof(type));
#if defined(NOS_ARM)
    if (wide) {
        struct flock64 output;
        u_memset(&output, 0, sizeof(output));
        output.l_type = type; output.l_whence = lock.whence;
        output.l_start = lock.start; output.l_len = lock.length; output.l_pid = lock.pid;
        return nos_user_copy((void *)pointer, &output, sizeof(output));
    }
    if (lock.start < INT32_MIN || lock.start > INT32_MAX ||
        lock.length < INT32_MIN || lock.length > INT32_MAX) return -EOVERFLOW;
#endif
    struct flock output;
    u_memset(&output, 0, sizeof(output));
    output.l_type = type; output.l_whence = lock.whence;
    output.l_start = lock.start; output.l_len = lock.length; output.l_pid = lock.pid;
    return nos_user_copy((void *)pointer, &output, sizeof(output));
}
static intptr_t gateway_fcntl(int fd, unsigned command, uintptr_t value)
{
    switch (command) {
    case F_GETLK: case F_SETLK: case F_SETLKW:
#if defined(NOS_ARM)
    case F_GETLK64: case F_SETLK64: case F_SETLKW64:
#endif
        return gateway_record_lock(fd, command, value);
    case F_DUPFD: return value > INT_MAX ? -EINVAL : nos_dup_min(fd, (unsigned)value);
    case F_GETFD: case F_SETFD: {
        if (command == F_SETFD && value & ~1u) return -EINVAL;
        struct nos_message message = { .op = command == F_GETFD ? NOS_VFS_GETFD : NOS_VFS_SETFD,
            .arg = fd, .value = (intptr_t)value };
        return nos_rpc(&message);
    }
    case F_DUPFD_CLOEXEC: {
        if (value > INT_MAX) return -EINVAL;
        struct nos_message message = { .op = NOS_VFS_DUP, .arg = fd, .value = -1,
            .len = value, .data = { NOS_FD_CLOEXEC } };
        return nos_rpc(&message);
    }
    case F_GETFL: {
        int native = nos_getflags(fd);
        if (native < 0) return native;
        unsigned flags = (native & NOS_O_RDWR) == NOS_O_RDWR ? O_RDWR :
                         native & NOS_O_WRONLY ? O_WRONLY : O_RDONLY;
        if (native & NOS_O_APPEND) flags |= O_APPEND;
        if (native & NOS_O_NONBLOCK) flags |= O_NONBLOCK;
        if (native & NOS_O_DIRECTORY) flags |= O_DIRECTORY;
        return flags;
    }
    case F_SETFL: {
        unsigned long allowed = O_ACCMODE | O_APPEND | O_NONBLOCK;
#ifdef O_LARGEFILE
        allowed |= O_LARGEFILE;
#endif
        if (value & ~allowed) return -ENOTSUP;
        return nos_setflags(fd, ((value & O_APPEND) ? NOS_O_APPEND : 0) |
            ((value & O_NONBLOCK) ? NOS_O_NONBLOCK : 0));
    }
    default: return -ENOTSUP;
    }
}
static int copy_linux_time(uintptr_t pointer, int wide, int64_t *seconds, long *nanoseconds)
{
    if (!pointer) { *seconds = -1; *nanoseconds = 0; return 0; }
    if (wide) {
        int64_t value[2];
        int rc = nos_user_copy(value, (const void *)pointer, sizeof(value));
        if (rc) return rc;
        if (value[0] < 0 || value[1] < 0 || value[1] >= 1000000000) return -EINVAL;
        *seconds = value[0]; *nanoseconds = (long)value[1];
    } else {
        int32_t value[2];
        int rc = nos_user_copy(value, (const void *)pointer, sizeof(value));
        if (rc) return rc;
        if (value[0] < 0 || value[1] < 0 || value[1] >= 1000000000) return -EINVAL;
        *seconds = value[0]; *nanoseconds = value[1];
    }
    return 0;
}
static intptr_t gateway_ppoll(uintptr_t descriptors, size_t count, uintptr_t pointer, uintptr_t mask,
                              uintptr_t mask_bytes, int wide)
{
    if (mask && mask_bytes != 8) return -EINVAL;
    int64_t seconds; long nanos;
    int rc = copy_linux_time(pointer, wide, &seconds, &nanos);
    if (rc) return rc;
    if (seconds > (INT64_MAX - nanos / 1000000 - 1) / 1000) return -EOVERFLOW;
    uint64_t milliseconds = seconds < 0 ? UINT64_MAX : (uint64_t)seconds * 1000 + (nanos + 999999) / 1000000;
    rc = (int)nos_syscall(NOS_SYS_LINUX_WAITCONTROL, 0, mask, 8);
    if (rc) return rc;
    for (;;) {
        int timeout = milliseconds == UINT64_MAX ? -1 : milliseconds > INT32_MAX ? INT32_MAX : (int)milliseconds;
        intptr_t result = nos_poll_gateway((struct nos_pollfd *)descriptors, count, timeout);
        if (result || milliseconds == UINT64_MAX || milliseconds <= INT32_MAX) return result;
        milliseconds -= INT32_MAX;
    }
}
static intptr_t gateway_pselect(const uintptr_t a[6], int wide)
{
    uintptr_t mask[2] = {0, 8};
    if (a[5]) {
        int rc = nos_user_copy(mask, (const void *)a[5], sizeof(mask));
        if (rc) return rc;
        if (mask[0] && mask[1] != 8) return -EINVAL;
    }
    int64_t seconds; long nanos;
    int rc = copy_linux_time(a[4], wide, &seconds, &nanos);
    if (rc) return rc;
    rc = (int)nos_syscall(NOS_SYS_LINUX_WAITCONTROL, 0, mask[0], 8);
    return rc ? rc : nos_select_gateway((int)a[0], a[1], a[2], a[3], seconds, nanos);
}
static intptr_t gateway_call(const struct nos_linux_call *call)
{
    const uintptr_t *a = call->args;
    switch (call->number) {
    case SYS_execve: {
        char path[NOS_PATH_MAX];
        intptr_t resolved = nos_resolve_exec_at(LINUX_AT_FDCWD, (const char *)a[0], path, sizeof(path));
        if (resolved < 0) return resolved;
        if (!resolved || (size_t)resolved > sizeof(path) || path[resolved - 1]) return -EIO;
        unsigned char random[16];
        intptr_t count = gateway_getrandom((uintptr_t)random, sizeof(random), 0);
        if (count < 0) return count;
        if (count != sizeof(random)) return -EIO;
        struct nos_exec_request request = { path,
            (const char *const *)a[1], (const char *const *)a[2], 0 };
        return nos_syscall(NOS_SYS_LINUX_EXEC, (uintptr_t)&request,
            (uintptr_t)random, NOS_EXEC_RANDOM_VALID);
    }
#ifdef SYS_open
    case SYS_open: return gateway_openat(LINUX_AT_FDCWD, a[0], a[1], (unsigned)a[2]);
#endif
    case SYS_openat:
        if (!fd_valid(a[0])) return -EBADF;
        return gateway_openat((int)a[0], a[1], a[2], (unsigned)a[3]);
    case SYS_mkdirat: return nos_mkdirat((int)a[0], (const char *)a[1], (unsigned)a[2]);
#ifdef SYS_mkdir
    case SYS_mkdir: return nos_mkdirat(LINUX_AT_FDCWD, (const char *)a[0], (unsigned)a[1]);
#endif
    case SYS_umask: return nos_umask((unsigned)a[0] & 0777u);
    case SYS_faccessat:
        return nos_access_at((int)a[0], (const char *)a[1], (unsigned)a[2], 0);
#ifdef SYS_faccessat2
    case SYS_faccessat2:
        return nos_access_at((int)a[0], (const char *)a[1], (unsigned)a[2], (unsigned)a[3]);
#endif
#ifdef SYS_access
    case SYS_access: return nos_access_at(LINUX_AT_FDCWD, (const char *)a[0], (unsigned)a[1], 0);
#endif
    case SYS_fchownat:
        return nos_chown_at((int)a[0], (const char *)a[1], (uint32_t)a[2], (uint32_t)a[3], (unsigned)a[4]);
#ifdef NOS_ARM
    /* The original ARM calls carry UID16/GID16. Real glibc uses the
     * explicitly named UID32 syscalls; leave the old ABI unsupported. */
    case SYS_chown32:
        return nos_chown_at(LINUX_AT_FDCWD, (const char *)a[0], (uint32_t)a[1], (uint32_t)a[2], 0);
    case SYS_lchown32:
        return nos_chown_at(LINUX_AT_FDCWD, (const char *)a[0], (uint32_t)a[1], (uint32_t)a[2], NOS_AT_SYMLINK_NOFOLLOW);
    case SYS_fchown32: return nos_fchown((int)a[0], (uint32_t)a[1], (uint32_t)a[2]);
#else
    case SYS_fchown: return nos_fchown((int)a[0], (uint32_t)a[1], (uint32_t)a[2]);
#ifdef SYS_chown
    case SYS_chown:
        return nos_chown_at(LINUX_AT_FDCWD, (const char *)a[0], (uint32_t)a[1], (uint32_t)a[2], 0);
    case SYS_lchown:
        return nos_chown_at(LINUX_AT_FDCWD, (const char *)a[0], (uint32_t)a[1], (uint32_t)a[2], NOS_AT_SYMLINK_NOFOLLOW);
#endif
#endif
    case SYS_fchmod: return nos_fchmod((int)a[0], (unsigned)a[1]);
    case SYS_fchmodat: return nos_chmod_at((int)a[0], (const char *)a[1], (unsigned)a[2]);
#ifdef SYS_fchmodat2
    case SYS_fchmodat2:
        if (a[3]) return -ENOTSUP;
        return nos_chmod_at((int)a[0], (const char *)a[1], (unsigned)a[2]);
#endif
#ifdef SYS_chmod
    case SYS_chmod: return nos_chmod_at(LINUX_AT_FDCWD, (const char *)a[0], (unsigned)a[1]);
#endif
    case SYS_ftruncate: return nos_ftruncate((int)a[0], (intptr_t)a[1]);
    case SYS_truncate: return nos_truncate((const char *)a[0], (intptr_t)a[1]);
#ifdef SYS_ftruncate64
    case SYS_ftruncate64: return nos_ftruncate((int)a[0], (int64_t)((uint64_t)a[2] | (uint64_t)a[3] << 32));
    case SYS_truncate64: return nos_truncate((const char *)a[0], (int64_t)((uint64_t)a[2] | (uint64_t)a[3] << 32));
#endif
    case SYS_symlinkat: return nos_symlink_at((const char *)a[0], (int)a[1], (const char *)a[2]);
    case SYS_linkat:
        if (a[4]) return -ENOTSUP;
        return nos_link_at((int)a[0], (const char *)a[1], (int)a[2], (const char *)a[3]);
#ifdef SYS_symlink
    case SYS_symlink: return nos_symlink_at((const char *)a[0], LINUX_AT_FDCWD, (const char *)a[1]);
    case SYS_link: return nos_link_at(LINUX_AT_FDCWD, (const char *)a[0], LINUX_AT_FDCWD, (const char *)a[1]);
#endif
    case SYS_close: return fd_valid(a[0]) ? nos_close((int)a[0]) : -EBADF;
    case SYS_unlinkat: {
        if (a[2] & ~LINUX_AT_REMOVEDIR) return -EINVAL;
        return nos_unlinkat((int)a[0], (const char *)a[1], (unsigned)a[2]);
    }
#ifdef SYS_unlink
    case SYS_unlink: return nos_unlink((const char *)a[0]);
    case SYS_rmdir: return nos_rmdir((const char *)a[0]);
#endif
#ifdef SYS_renameat
    case SYS_renameat:
        return nos_renameat((int)a[0], (const char *)a[1], (int)a[2], (const char *)a[3]);
#endif
#ifdef SYS_renameat2
    case SYS_renameat2:
        if (a[4]) return -ENOTSUP;
        return nos_renameat((int)a[0], (const char *)a[1], (int)a[2], (const char *)a[3]);
#endif
#ifdef SYS_rename
    case SYS_rename: return nos_rename((const char *)a[0], (const char *)a[1]);
#endif
    case SYS_read: case SYS_write:
        if (!fd_valid(a[0])) return -EBADF;
        if (a[2] > INTPTR_MAX) return -EINVAL;
        return call->number == SYS_read ? nos_posix_read((int)a[0], (void *)a[1], a[2]) :
            nos_posix_write((int)a[0], (const void *)a[1], a[2]);
    case SYS_readv: case SYS_writev:
        return !fd_valid(a[0]) ? -EBADF : vector_io((int)a[0], a[1], a[2], call->number == SYS_writev);
    case SYS_pread64: case SYS_pwrite64: {
        if (!fd_valid(a[0])) return -EBADF;
        if (a[2] > INTPTR_MAX) return -EINVAL;
#if defined(NOS_ARM)
        uint64_t offset = (uint64_t)a[4] | (uint64_t)a[5] << 32;
#else
        uint64_t offset = a[3];
#endif
        if (offset > INT64_MAX) return -EINVAL;
        return call->number == SYS_pread64 ? nos_pread((int)a[0], (void *)a[1], a[2], offset) :
            nos_pwrite((int)a[0], (const void *)a[1], a[2], offset);
    }
    case SYS_lseek: return !fd_valid(a[0]) ? -EBADF : nos_seek((int)a[0], (intptr_t)a[1], (int)a[2]);
#ifdef SYS__llseek
    case SYS__llseek: {
        int64_t displacement = (int64_t)((uint64_t)a[1] << 32 | a[2]);
        if (displacement > INTPTR_MAX || displacement < INTPTR_MIN) return -EOVERFLOW;
        intptr_t rc = nos_seek((int)a[0], (intptr_t)displacement, (int)a[4]);
        if (rc < 0) return rc;
        uint64_t offset = (uintptr_t)rc;
        return nos_user_copy((void *)a[3], &offset, sizeof(offset));
    }
#endif
#ifdef SYS_fstat64
    case SYS_fstat64: return gateway_stat(0, 0, (int)a[0], a[1], 0, 1);
#endif
#ifdef SYS_fstatat64
    case SYS_fstatat64: return gateway_stat((int)a[0], a[1], -1, a[2], (unsigned)a[3], 1);
#endif
    case SYS_fstat: return gateway_stat(0, 0, (int)a[0], a[1], 0, 0);
#ifdef SYS_newfstatat
    case SYS_newfstatat: return gateway_stat((int)a[0], a[1], -1, a[2], (unsigned)a[3], 0);
#endif
#ifdef SYS_stat
    case SYS_stat: return gateway_stat(LINUX_AT_FDCWD, a[0], -1, a[1], 0, 0);
    case SYS_lstat: return gateway_stat(LINUX_AT_FDCWD, a[0], -1, a[1], LINUX_AT_SYMLINK_NOFOLLOW, 0);
#endif
    case SYS_statx: return gateway_statx((int)a[0], a[1], (unsigned)a[2], a[4]);
    case SYS_getdents64: return nos_getdents((int)a[0], (void *)a[1], a[2]);
    case SYS_getcwd: return nos_getcwd((char *)a[0], a[1]);
    case SYS_chdir: return nos_chdir((const char *)a[0]);
    case SYS_fchdir: return nos_fchdir((int)a[0]);
    case SYS_readlinkat: return nos_readlink_at((int)a[0], (const char *)a[1], (char *)a[2], a[3]);
#ifdef SYS_readlink
    case SYS_readlink: return nos_readlink_at(LINUX_AT_FDCWD, (const char *)a[0], (char *)a[1], a[2]);
#endif
    case SYS_dup: return nos_dup((int)a[0]);
#ifdef SYS_dup2
    case SYS_dup2: return nos_dup2((int)a[0], (int)a[1]);
#endif
    case SYS_dup3:
        if (a[0] == a[1]) return -EINVAL;
        if (a[2] & ~O_CLOEXEC) return -EINVAL;
        {
            struct nos_message message = { .op = NOS_VFS_DUP, .arg = (int)a[0], .value = (int)a[1],
                .data = { (a[2] & O_CLOEXEC) ? NOS_FD_CLOEXEC : 0 } };
            return nos_rpc(&message);
        }
    case SYS_fcntl: return gateway_fcntl((int)a[0], (unsigned)a[1], a[2]);
#ifdef SYS_fcntl64
    case SYS_fcntl64: return gateway_fcntl((int)a[0], (unsigned)a[1], a[2]);
#endif
    case SYS_pipe2:
        if (a[1] & ~(O_NONBLOCK | O_CLOEXEC)) return -EINVAL;
        return nos_pipe2((int *)a[0], ((a[1] & O_NONBLOCK) ? NOS_O_NONBLOCK : 0) |
            ((a[1] & O_CLOEXEC) ? NOS_O_CLOEXEC : 0));
#ifdef SYS_pipe
    case SYS_pipe: return nos_pipe((int *)a[0]);
#endif
    case SYS_getrandom: return gateway_getrandom(a[0], a[1], (unsigned)a[2]);
#ifdef SYS_poll
    case SYS_poll: {
        intptr_t result = nos_syscall(NOS_SYS_LINUX_WAITCONTROL, 0, 0, 8);
        return result ? result : nos_poll_gateway((struct nos_pollfd *)a[0], a[1], (int)a[2]);
    }
#endif
    case SYS_ppoll: return gateway_ppoll(a[0], a[1], a[2], a[3], a[4], sizeof(uintptr_t) == 8);
    case SYS_pselect6: return gateway_pselect(a, sizeof(uintptr_t) == 8);
#ifdef SYS_ppoll_time64
    case SYS_ppoll_time64: return gateway_ppoll(a[0], a[1], a[2], a[3], a[4], 1);
#endif
#ifdef SYS_pselect6_time64
    case SYS_pselect6_time64: return gateway_pselect(a, 1);
#endif
#ifdef SYS_mmap2
    case SYS_mmap2: return gateway_mmap(a[0], a[1], (unsigned)a[2], (unsigned)a[3], (int)a[4], (uint64_t)a[5] << 12);
#else
    case SYS_mmap:
#if defined(NOS_ARM)
    {
        /* ARM's original mmap syscall receives an argument-block pointer;
         * mmap2 above receives registers and an offset measured in pages. */
        uintptr_t arguments[6];
        int rc = nos_user_copy(arguments, (const void *)a[0], sizeof(arguments));
        return rc ? rc : gateway_mmap(arguments[0], arguments[1], (unsigned)arguments[2],
            (unsigned)arguments[3], (int)arguments[4], arguments[5]);
    }
#else
        return gateway_mmap(a[0], a[1], (unsigned)a[2], (unsigned)a[3], (int)a[4], a[5]);
#endif
#endif
    case SYS_ioctl: return -ENOTTY;
    case SYS_fsync: case SYS_fdatasync: {
        struct nos_stat st;
        int rc = nos_stat_fd((int)a[0], &st);
        return rc ? rc : nos_sync();
    }
    default: return -ENOSYS;
    }
}
void nos_linux_gateway(struct nos_linux_call *pointer)
{
    struct nos_linux_call call;
    intptr_t result = nos_user_copy(&call, pointer, sizeof(call));
    if (!result) result = gateway_call(&call);
    nos_syscall(NOS_SYS_LINUX_RETURN, (uintptr_t)result, 0, 0);
    /* RETURN must restore the saved frame. It must never return into a
     * completed upcall, including when a malicious request corrupted state. */
    nos_syscall(NOS_SYS_EXIT_GROUP, (uintptr_t)-EFAULT, 0, 0);
    for (;;) { }
}
