/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
#include "user.h"

/* Descriptor operations, validated transfer loops, and blocking libc adapters. */

/* Descriptor metadata and lifecycle. */
int nos_ftruncate(int fd, int64_t length)
{
    if (length < 0)
        return -NOS_EINVAL;
    struct nos_message m = { .op = NOS_VFS_FTRUNCATE, .arg = fd, .len = sizeof(length) };
    u_memcpy(m.data, &length, sizeof(length));
    return (int)nos_rpc(&m);
}

int nos_umask(unsigned mask)
{
    struct nos_message m = { .op = NOS_VFS_UMASK, .value = mask & 0777u };
    return (int)nos_rpc(&m);
}

int nos_fchmod(int fd, unsigned mode)
{
    struct nos_message m = { .op = NOS_VFS_FCHMOD, .arg = fd, .value = mode & 07777u };
    return (int)nos_rpc(&m);
}

int nos_fchown(int fd, uint32_t uid, uint32_t gid)
{
#ifdef NOS_LINUX_COMPAT
    uint32_t ids[2] = {uid, gid};
    struct nos_message m = { .op = NOS_VFS_FCHOWN, .arg = fd, .len = sizeof(ids) };
    u_memcpy(m.data, ids, sizeof(ids));
    return (int)nos_rpc(&m);
#else
    (void)fd;
    (void)uid;
    (void)gid;
    return -NOS_ENOTSUP;
#endif
}

int nos_close(int fd)
{
    struct nos_message m = { .op = NOS_VFS_CLOSE, .arg = fd };
    return (int)nos_rpc(&m);
}

/* Sequential transfers. */
static intptr_t read_mode(int fd, void *buffer, size_t count, unsigned mode)
{
    int check = nos_user_check(buffer, count, NOS_PROT_WRITE);
    if (check)
        return check;
    size_t done = 0;
    do {
        struct nos_message m = { .op = NOS_VFS_READ, .arg = fd,
            .value = mode || done ? NOS_IO_DONTWAIT : 0 };
        size_t requested = count - done < NOS_MSG_DATA ? count - done : NOS_MSG_DATA;
        m.len = requested;
#ifdef NOS_LINUX_COMPAT
        /* A pipe stores directly to this final span before its successful
         * reply commits consumption. Other descriptors still reply inline. */
        m.bulk_out = (uintptr_t)buffer + done;
        m.bulk_out_len = requested;
#endif
        intptr_t n = nos_rpc(&m);
        if (n < 0)
            return done ? (intptr_t)done : n;
        if (!n)
            break;
        if ((size_t)n > requested || m.len > NOS_MSG_DATA)
            return done ? (intptr_t)done : -NOS_EIO;
#ifdef NOS_LINUX_COMPAT
        if (m.len)
#endif
        {
            if ((size_t)n > m.len)
                return done ? (intptr_t)done : -NOS_EIO;
            int rc = nos_user_copy((char *)buffer + done, m.data, (size_t)n);
            if (rc)
                return done ? (intptr_t)done : rc;
        }
        done += (size_t)n;
        if ((size_t)n < requested)
            break;
    } while (done < count);
    return (intptr_t)done;
}

intptr_t nos_read(int fd, void *buffer, size_t count)
{
    return read_mode(fd, buffer, count, 0);
}

intptr_t nos_read_nowait(int fd, void *buffer, size_t count)
{
    return read_mode(fd, buffer, count, 1);
}

static intptr_t write_mode(int fd, const void *buffer, size_t count, unsigned mode)
{
    int check = nos_user_check(buffer, count, NOS_PROT_READ);
    if (check)
        return check;
    size_t done = 0;
    do {
        struct nos_message m = { .op = NOS_VFS_WRITE, .arg = fd,
#ifdef NOS_LINUX_COMPAT
            /* A blocking large write must keep waiting after its first chunk.
             * NONBLOCK is an OFD flag, while mode requests per-call DONTWAIT. */
            .value = (mode & NOS_IO_DONTWAIT) |
                (count > NOS_PIPE_BUF || (mode & NOS_IO_WRITE_LARGE) ? NOS_IO_WRITE_LARGE : 0) };
        size_t requested = count - done < NOS_PIPE_BUF ? count - done : NOS_PIPE_BUF;
        m.bulk_in = (uintptr_t)buffer + done;
        m.bulk_in_len = requested;
#else
            .value = mode || done ? NOS_IO_DONTWAIT : 0 };
        size_t requested = count - done < NOS_MSG_DATA ? count - done : NOS_MSG_DATA;
        m.len = requested;
        int rc = nos_user_copy(m.data, (const char *)buffer + done, requested);
        if (rc)
            return done ? (intptr_t)done : rc;
#endif
        intptr_t n = nos_rpc(&m);
#ifdef NOS_LINUX_COMPAT
        /* A later chunk can fail after a prefix was committed. Preserve the
         * byte count while queuing SIGPIPE for delivery at LINUX_RETURN. */
        if (n == -NOS_EPIPE)
            (void)nos_syscall(NOS_SYS_LINUX_RAISE, 13, 0, 0);
#endif
        if (n < 0)
            return done ? (intptr_t)done : n;
        if (!n)
            break;
        if ((size_t)n > requested)
            return done ? (intptr_t)done : -NOS_EIO;
        done += (size_t)n;
        if ((size_t)n < requested)
            break;
    } while (done < count);
    return (intptr_t)done;
}

intptr_t nos_write(int fd, const void *buffer, size_t count)
{
    return write_mode(fd, buffer, count, 0);
}

intptr_t nos_write_nowait(int fd, const void *buffer, size_t count)
{
    return write_mode(fd, buffer, count, 1);
}

intptr_t nos_seek(int fd, intptr_t offset, int whence)
{
    struct nos_message m = { .op = NOS_VFS_SEEK, .arg = fd, .value = offset, .len = (size_t)whence };
    return nos_rpc(&m);
}

/* Advisory record locks and descriptor status. */
int nos_record_lock(int fd, unsigned command, struct nos_record_lock *lock)
{
#ifdef NOS_LINUX_COMPAT
    _Static_assert(sizeof(struct nos_record_lock) == 32, "record lock IPC layout");
    if (command > NOS_LOCK_SET_WAIT)
        return -NOS_EINVAL;
    if (command == NOS_LOCK_GET) {
        int rc = nos_user_check(lock, sizeof(*lock), NOS_PROT_WRITE);
        if (rc)
            return rc;
    }
    struct nos_record_lock input;
    int rc = nos_user_copy(&input, lock, sizeof(input));
    if (rc)
        return rc;
    if (input.reserved)
        return -NOS_EINVAL;
    struct nos_message message = { .op = NOS_VFS_RECORD_LOCK, .arg = fd,
        .value = command, .len = sizeof(input) };
    u_memcpy(message.data, &input, sizeof(input));
    intptr_t result = nos_rpc(&message);
    if (result || command != NOS_LOCK_GET)
        return (int)result;
    if (message.len != sizeof(input))
        return -NOS_EIO;
    return nos_user_copy(lock, message.data, sizeof(input));
#else
    (void)fd;
    (void)command;
    (void)lock;
    return -NOS_ENOTSUP;
#endif
}

int nos_stat_fd(int fd, struct nos_stat *st)
{
    struct nos_message m = { .op = NOS_VFS_STAT, .arg = fd };
    intptr_t rc = nos_rpc(&m);
    if (!rc)
        rc = nos_user_copy(st, m.data, sizeof(*st));
    return (int)rc;
}

int nos_stat64_fd(int fd, struct nos_stat64 *st)
{
    int rc = nos_user_check(st, sizeof(*st), NOS_PROT_WRITE);
    if (rc)
        return rc;
    struct nos_message m = { .op = NOS_VFS_STAT64, .arg = fd };
    intptr_t result = nos_rpc(&m);
    if (result)
        return (int)result;
    if (m.len != sizeof(*st))
        return -NOS_EIO;
    return nos_user_copy(st, m.data, sizeof(*st));
}
/* Positioned transfers. */
#ifdef NOS_LINUX_COMPAT
#define NOS_PREAD_BULK_BYTES 512u
_Static_assert(NOS_PREAD_BULK_BYTES <= NOS_IPC_BULK_MAX, "pread scratch fits IPC bulk");
static intptr_t positioned_bulk_read(int fd, void *buffer, size_t count, uint64_t offset)
{
    unsigned char scratch[NOS_PREAD_BULK_BYTES];
    size_t done = 0;
    while (done < count) {
        size_t wanted = count - done < sizeof(scratch) ? count - done : sizeof(scratch);
        struct nos_message message = {.op = NOS_VFS_PREAD, .arg = fd, .len = sizeof(offset),
            .bulk_out = (uintptr_t)scratch, .bulk_out_len = wanted};
        uint64_t position = offset + done;
        u_memcpy(message.data, &position, sizeof(position));
        intptr_t result = nos_rpc(&message);
        if (result < 0)
            return done ? (intptr_t)done : result;
        if ((size_t)result > wanted || message.len)
            return done ? (intptr_t)done : -NOS_EIO;
        /* A STORE may precede a cancelled or failed reply.  Only scratch is
         * exposed to the server;
         publish this block after a successful RPC. */
        if (result) {
            int rc = nos_user_copy((char *)buffer + done, scratch, (size_t)result);
            if (rc)
                return done ? (intptr_t)done : rc;
        }
        done += (size_t)result;
        if ((size_t)result < wanted)
            break;
    }
    return (intptr_t)done;
}
#endif
static intptr_t positioned_io(int fd, void *buffer, size_t count, int64_t offset, int write)
{
    if (offset < 0)
        return -NOS_EINVAL;
    if (count > (size_t)INTPTR_MAX || (uint64_t)count > INT64_MAX - (uint64_t)offset)
        return -NOS_EOVERFLOW;
    int rc = nos_user_check(buffer, count, write ? NOS_PROT_READ : NOS_PROT_WRITE);
    if (rc)
        return rc;
#ifdef NOS_LINUX_COMPAT
    if (!write && count > NOS_MSG_DATA)
        return positioned_bulk_read(fd, buffer, count, (uint64_t)offset);
#endif
    size_t done = 0;
    do {
        size_t limit = write ? NOS_MSG_DATA - 8 : NOS_MSG_DATA;
        size_t wanted = count - done < limit ? count - done : limit;
        struct nos_message m = { .op = write ? NOS_VFS_PWRITE : NOS_VFS_PREAD,
            .arg = fd, .len = wanted + (write ? 8 : 0) };
        uint64_t position = (uint64_t)offset + done;
        u_memcpy(m.data, &position, sizeof(position));
        if (write && (rc = nos_user_copy(m.data + 8, (char *)buffer + done, wanted)))
            return done ? (intptr_t)done : rc;
        intptr_t n = nos_rpc(&m);
        if (n < 0)
            return done ? (intptr_t)done : n;
        if ((size_t)n > wanted || (!write && (size_t)n > m.len))
            return done ? (intptr_t)done : -NOS_EIO;
        if (!write && (rc = nos_user_copy((char *)buffer + done, m.data, (size_t)n)))
            return done ? (intptr_t)done : rc;
        done += (size_t)n;
        if ((size_t)n < wanted)
            break;
    } while (done < count);
    return (intptr_t)done;
}

intptr_t nos_pread(int fd, void *buffer, size_t count, int64_t offset)
{
    return positioned_io(fd, buffer, count, offset, 0);
}

intptr_t nos_pwrite(int fd, const void *buffer, size_t count, int64_t offset)
{
    return positioned_io(fd, (void *)buffer, count, offset, 1);
}

/* Directory records and descriptor control. */
intptr_t nos_getdents(int fd, void *buffer, size_t capacity)
{
    if (capacity > (size_t)INTPTR_MAX)
        return -NOS_EOVERFLOW;
    int rc = nos_user_check(buffer, capacity, NOS_PROT_WRITE);
    if (rc)
        return rc;
    if (!capacity)
        return -NOS_EINVAL;
    size_t done = 0;
    for (;;) {
        unsigned char record[NOS_DIRENT_RECORD_MAX];
        size_t copied = 0, length = 0;
        uint64_t cookie = 0;
        do {
            struct nos_message m = { .op = NOS_VFS_GETDENTS, .arg = fd,
                .value = (intptr_t)copied, .len = NOS_MSG_DATA };
            uint64_t available = capacity - done;
            u_memcpy(m.data, &cookie, sizeof(cookie));
            u_memcpy(m.data + 8, &available, sizeof(available));
            intptr_t result = nos_rpc(&m);
            if (result < 0)
                return done ? (intptr_t)done : result;
            if (!result)
                return (intptr_t)done;
            if (result < 24 || result > (intptr_t)sizeof(record) ||
                (size_t)result > capacity - done || !m.len || m.len > NOS_MSG_DATA ||
                copied + m.len > (size_t)result || (length && length != (size_t)result))
                return done ? (intptr_t)done : -NOS_EIO;
            length = (size_t)result;
            cookie = (uint64_t)(uintptr_t)m.arg;
            u_memcpy(record + copied, m.data, m.len);
            copied += m.len;
        } while (copied < length);
        uint16_t reclen;
        u_memcpy(&reclen, record + 16, sizeof(reclen));
        if (reclen != length || record[length - 1])
            return done ? (intptr_t)done : -NOS_EIO;
        rc = nos_user_copy((char *)buffer + done, record, length);
        if (rc)
            return done ? (intptr_t)done : rc;
        done += length;
        if (capacity - done < 24)
            return (intptr_t)done;
    }
}

int nos_fchdir(int fd)
{
    struct nos_message m = { .op = NOS_VFS_FCHDIR, .arg = fd };
    return (int)nos_rpc(&m);
}

int nos_readdir(int fd, size_t index, char *name, size_t size)
{
    size_t span = size < NOS_PATH_MAX ? size : NOS_PATH_MAX;
    int rc = nos_user_check(name, span, NOS_PROT_WRITE);
    if (rc)
        return rc;
    struct nos_message m = { .op = NOS_VFS_READDIR, .arg = fd, .value = (intptr_t)index };
#ifdef NOS_LINUX_COMPAT
    if (span) { m.bulk_out = (uintptr_t)name; m.bulk_out_len = span; }
#endif
    intptr_t result = nos_rpc(&m);
    if (result <= 0)
        return (int)result;
#ifdef NOS_LINUX_COMPAT
    if (span) {
        if (m.len)
            return -NOS_EIO;
        for (size_t i = 0; i < span; i++) {
            char byte;
            rc = nos_user_copy(&byte, name + i, 1);
            if (rc)
                return rc;
            if (!byte)
                return 1;
        }
        return -NOS_EIO;
    }
#endif
    if (!m.len || m.len > sizeof(m.data) || m.data[m.len - 1])
        return -NOS_EIO;
    if (m.len > size)
        return -NOS_ENAMETOOLONG;
    rc = nos_user_copy(name, m.data, m.len);
    return rc ? rc : 1;
}

int nos_dup2(int fd, int target)
{
    if (target < 0)
        return -NOS_EBADF;
    struct nos_message m = { .op = NOS_VFS_DUP, .arg = fd, .value = target };
    return (int)nos_rpc(&m);
}

int nos_dup3(int fd, int target, unsigned flags)
{
    if (fd == target || (flags & ~NOS_O_CLOEXEC))
        return -NOS_EINVAL;
    if (target < 0)
        return -NOS_EBADF;
    struct nos_message m = { .op = NOS_VFS_DUP, .arg = fd, .value = target };
    m.data[0] = flags & NOS_O_CLOEXEC ? 1 : 0;
    return (int)nos_rpc(&m);
}

int nos_dup_min(int fd, unsigned minimum)
{
    struct nos_message m = { .op = NOS_VFS_DUP, .arg = fd, .value = -1, .len = minimum };
    return (int)nos_rpc(&m);
}

int nos_dup(int fd)
{
#ifdef NOS_LINUX_COMPAT
    /* Plain dup and F_DUPFD(0) differ when the soft limit is zero. */
    struct nos_message m = { .op = NOS_VFS_DUP, .arg = fd, .value = -2 };
    return (int)nos_rpc(&m);
#else
    return nos_dup_min(fd, 0);
#endif
}

int nos_dup_cloexec(int fd, unsigned minimum)
{
    struct nos_message m = { .op = NOS_VFS_DUP, .arg = fd, .value = -1, .len = minimum };
    m.data[0] = 1;
    return (int)nos_rpc(&m);
}

int nos_getflags(int fd)
{
    struct nos_message m = { .op = NOS_VFS_GETFLAGS, .arg = fd };
    return (int)nos_rpc(&m);
}

int nos_setflags(int fd, unsigned flags)
{
    struct nos_message m = { .op = NOS_VFS_SETFLAGS, .arg = fd, .value = flags };
    return (int)nos_rpc(&m);
}

int nos_getfd(int fd)
{
    struct nos_message m = { .op = NOS_VFS_GETFD, .arg = fd };
    return (int)nos_rpc(&m);
}

int nos_setfd(int fd, unsigned flags)
{
    struct nos_message m = { .op = NOS_VFS_SETFD, .arg = fd, .value = flags };
    return (int)nos_rpc(&m);
}

int nos_pipe(int descriptors[2])
{
    return nos_pipe2(descriptors, 0);
}

int nos_pipe2(int descriptors[2], unsigned flags)
{
    int rc = nos_user_check(descriptors, 2 * sizeof(int), NOS_PROT_WRITE);
    if (rc)
        return rc;
    struct nos_message m = { .op = NOS_VFS_PIPE, .arg = flags };
    intptr_t result = nos_rpc(&m);
    if (result)
        return (int)result;
    if (m.len != 2 * sizeof(int))
        return -NOS_EIO;
    return nos_user_copy(descriptors, m.data, 2 * sizeof(int));
}

int nos_sync(void)
{
    struct nos_message m = { .op = NOS_VFS_SYNC };
    return (int)nos_rpc(&m);
}

/* VFS returns EAGAIN rather than blocking its service loop. The libc-facing
 * path implements blocking descriptors while the native API stays explicit. */
intptr_t nos_posix_read(int fd, void *buffer, size_t count)
{
    for (;;) {
        intptr_t rc = nos_read(fd, buffer, count);
        if (rc != -NOS_EAGAIN)
            return rc;
        int flags = nos_getflags(fd);
        if (flags < 0)
            return flags;
        if (flags & NOS_O_NONBLOCK)
            return -NOS_EAGAIN;
        rc = nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
        if (rc < 0)
            return rc;
    }
}

static intptr_t posix_write_mode(int fd, const void *buffer, size_t count, unsigned mode)
{
    for (;;) {
        intptr_t rc = write_mode(fd, buffer, count, mode);
        if (rc != -NOS_EAGAIN)
            return rc;
        int flags = nos_getflags(fd);
        if (flags < 0)
            return flags;
        if (flags & NOS_O_NONBLOCK)
            return -NOS_EAGAIN;
        rc = nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
        if (rc < 0)
            return rc;
    }
}

intptr_t nos_posix_write(int fd, const void *buffer, size_t count)
{
    return posix_write_mode(fd, buffer, count, 0);
}
#ifdef NOS_LINUX_COMPAT
/* Preserve the original writev size class even when an individual iovec is
 * smaller than PIPE_BUF. LARGE permits a nonblocking prefix;
 it is not NOWAIT. */
intptr_t nos_posix_write_large(int fd, const void *buffer, size_t count)
{
    return posix_write_mode(fd, buffer, count, NOS_IO_WRITE_LARGE);
}
#endif
