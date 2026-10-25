/* Copyright (C) 2026 NOS contributors. Newlib filesystem hooks over NOS IPC. */
#include "private.h"
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdarg.h>
#include "esp_vfs.h"
#include "esp_private/startup_internal.h"
#include "nos/driver/esp_idf.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#ifndef O_BINARY
#define O_BINARY 0
#endif
extern int __real__fcntl_r(struct _reent *, int, int, int);
extern int __real_ftruncate(int, off_t);
extern int __real_fsync(int);
extern ssize_t __real_pread(int, void *, size_t, off_t);
extern ssize_t __real_pwrite(int, const void *, size_t, off_t);
_Static_assert(FD_SETSIZE > NOS_FD_LIMIT, "SDK descriptors need a separate range");
#ifdef CONFIG_LWIP_MAX_SOCKETS
_Static_assert(FD_SETSIZE - CONFIG_LWIP_MAX_SOCKETS >= NOS_FD_LIMIT,
               "SDK socket range overlaps NOS descriptors");
#endif

static unsigned active;
int nos_libc_pid(void) {
    return (int)nos_syscall(NOS_SYS_GETPID, 0, 0, 0);
}
int nos_libc_context(struct _reent *r) {
    if (!active)
        return 0;
    if (r != __getreent())
        return -NOS_EDEADLK;
    int pid = nos_libc_pid();
    if (pid < 0)
        return pid;
    unsigned slot = (unsigned)pid % NOS_TASKS;
    if (nos_idf_device_domain_owned())
        return -NOS_EDEADLK;
    /* VFS cannot RPC to itself; idle must not create a process file table. */
    return !slot || slot == NOS_IDLE_FIRST ? 0 : 1;
}
intptr_t nos_libc_result(struct _reent *r, intptr_t rc) {
    if (rc >= 0)
        return rc;
    int error = (int)-rc;
    switch (error) {
    case NOS_EDEADLK:
        error = EDEADLK;
        break;
    case NOS_ENOLCK:
        error = ENOLCK;
        break;
    case NOS_ENAMETOOLONG:
        error = ENAMETOOLONG;
        break;
    case NOS_ENOSYS:
        error = ENOSYS;
        break;
    case NOS_ENOTEMPTY:
        error = ENOTEMPTY;
        break;
    case NOS_EOVERFLOW:
        error = EOVERFLOW;
        break;
    case NOS_ENOTSUP:
        error = ENOTSUP;
        break;
    case NOS_ETIMEDOUT:
        error = ETIMEDOUT;
        break;
    case 40:
        error = ELOOP;
        break;
    }
    r->_errno = error;
    return -1;
}
/* The SDK still owns global socket/device descriptors outside the reserved
 * native range. Reserve it before SDK stdio opens 0..2, avoiding collisions. */
ESP_SYSTEM_INIT_FN(nos_fd_range, CORE, BIT(0), 114) {
    const esp_vfs_t reserved = {.flags = ESP_VFS_FLAG_DEFAULT};
    return esp_vfs_register_fd_range(&reserved, NULL, 3, NOS_FD_LIMIT);
}
int nos_esp_libc_activate(void) {
    active = 1;
    if (nos_idf_device_domain_owned())
        return 0;
    return nos_esp_libc_task_enter();
}
static int flags_to_native(int flags, unsigned *out) {
    unsigned supported = O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND | O_EXCL | O_NONBLOCK |
                         O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY | O_BINARY;
    if ((unsigned)flags & ~supported || (flags & O_ACCMODE) == O_ACCMODE)
        return -NOS_ENOTSUP;
    unsigned result = flags & O_ACCMODE;
    if (flags & O_CREAT)
        result |= NOS_O_CREAT;
    if (flags & O_TRUNC)
        result |= NOS_O_TRUNC;
    if (flags & O_APPEND)
        result |= NOS_O_APPEND;
    if (flags & O_EXCL)
        result |= NOS_O_EXCL;
    if (flags & O_NONBLOCK)
        result |= NOS_O_NONBLOCK;
    if (flags & O_CLOEXEC)
        result |= NOS_O_CLOEXEC;
    if (flags & O_NOFOLLOW)
        result |= NOS_O_NOFOLLOW;
    if (flags & O_DIRECTORY)
        result |= NOS_O_DIRECTORY;
    *out = result;
    return 0;
}
int __wrap__open_r(struct _reent *r, const char *path, int flags, int mode) {
    int context = nos_libc_context(r);
    if (!context)
        return esp_vfs_open(r, path, flags, mode);
    if (context < 0)
        return (int)nos_libc_result(r, context);
    unsigned native;
    int rc = flags_to_native(flags, &native);
    return (int)nos_libc_result(r, rc ? rc : nos_openat_mode(NOS_AT_FDCWD, path, native, mode));
}
int __wrap__close_r(struct _reent *r, int fd) {
    int context = nos_libc_context(r);
    if (!context || fd >= (int)NOS_FD_LIMIT)
        return esp_vfs_close(r, fd);
    return (int)nos_libc_result(r, context < 0 ? context : nos_close(fd));
}
ssize_t __wrap__read_r(struct _reent *r, int fd, void *buffer, size_t size) {
    int context = nos_libc_context(r);
    if (!context || fd >= (int)NOS_FD_LIMIT)
        return esp_vfs_read(r, fd, buffer, size);
    return (ssize_t)nos_libc_result(r, context < 0 ? context : nos_posix_read(fd, buffer, size));
}
intptr_t nos_libc_write(struct _reent *r, int fd, const void *buffer, size_t size) {
    /* A driver may log while holding the device domain. Calling VFS here
     * would make VFS wait for that same domain. Keep diagnostic stdout/stderr
     * on the managed console in the owning task, using its recursive lock. */
    if (active && (fd == 1 || fd == 2) && r == __getreent() && nos_idf_device_domain_owned()) {
        int rc = nos_user_check(buffer, size, NOS_PROT_READ);
        if (rc)
            return nos_libc_result(r, rc);
        struct nos_io_buffer b = {(void *)buffer, size > 4096 ? 4096 : size, 0};
        for (unsigned retry = 0;; retry++) {
            rc = nos_idf_console_call(NOS_IO_WRITE, &b, sizeof(b));
            if (rc != -NOS_DRV_EAGAIN || retry == 10)
                break;
            vTaskDelay(1);
        }
        return nos_libc_result(r, rc ? rc : (intptr_t)b.actual);
    }
    int context = nos_libc_context(r);
    if (!context || fd >= (int)NOS_FD_LIMIT)
        return esp_vfs_write(r, fd, buffer, size);
    return (ssize_t)nos_libc_result(r, context < 0 ? context : nos_posix_write(fd, buffer, size));
}
ssize_t __wrap__write_r(struct _reent *r, int fd, const void *buffer, size_t size) {
    return (ssize_t)nos_libc_write(r, fd, buffer, size);
}
off_t __wrap__lseek_r(struct _reent *r, int fd, off_t offset, int whence) {
    int context = nos_libc_context(r);
    if (!context || fd >= (int)NOS_FD_LIMIT)
        return esp_vfs_lseek(r, fd, offset, whence);
    return (off_t)nos_libc_result(r, context < 0 ? context : nos_seek(fd, offset, whence));
}
static int stat_copy(struct _reent *r, const struct nos_stat64 *info, struct stat *target) {
    struct stat value = {0};
    value.st_ino = info->ino;
    value.st_mode = info->mode;
    value.st_size = info->size;
    value.st_nlink = info->nlink;
    value.st_uid = info->uid;
    value.st_gid = info->gid;
    value.st_blksize = 512;
    value.st_blocks = info->blocks;
    value.st_atime = info->atime_sec;
    value.st_mtime = info->mtime_sec;
    value.st_ctime = info->ctime_sec;
    if (value.st_size < 0 || (uint64_t)value.st_size != info->size)
        return (int)nos_libc_result(r, -NOS_EOVERFLOW);
    return (int)nos_libc_result(r, nos_user_copy(target, &value, sizeof(value)));
}
int __wrap__fstat_r(struct _reent *r, int fd, struct stat *target) {
    int context = nos_libc_context(r);
    if (!context || fd >= (int)NOS_FD_LIMIT)
        return esp_vfs_fstat(r, fd, target);
    struct nos_stat64 info;
    int rc = context < 0 ? context : nos_stat64_fd(fd, &info);
    return rc ? (int)nos_libc_result(r, rc) : stat_copy(r, &info, target);
}
int __wrap__stat_r(struct _reent *r, const char *path, struct stat *target) {
    int context = nos_libc_context(r);
    if (!context)
        return esp_vfs_stat(r, path, target);
    struct nos_stat64 info;
    int rc = context < 0 ? context : nos_stat_at(NOS_AT_FDCWD, path, 0, &info);
    return rc ? (int)nos_libc_result(r, rc) : stat_copy(r, &info, target);
}
int __wrap__isatty_r(struct _reent *r, int fd) {
    struct stat info;
    if (__wrap__fstat_r(r, fd, &info))
        return 0;
    if (S_ISCHR(info.st_mode))
        return 1;
    r->_errno = ENOTTY;
    return 0;
}
int __wrap__fcntl_r(struct _reent *r, int fd, int command, int argument) {
    int context = nos_libc_context(r);
    if (!context || fd >= (int)NOS_FD_LIMIT)
        return __real__fcntl_r(r, fd, command, argument);
    if (context < 0)
        return (int)nos_libc_result(r, context);
    intptr_t rc;
    switch (command) {
    case F_DUPFD:
        rc = argument < 0 ? -NOS_EINVAL : nos_dup_min(fd, argument);
        break;
    case F_DUPFD_CLOEXEC:
        rc = argument < 0 ? -NOS_EINVAL : nos_dup_cloexec(fd, argument);
        break;
    case F_GETFD:
        rc = nos_getfd(fd);
        break;
    case F_SETFD:
        rc = argument & ~FD_CLOEXEC ? -NOS_EINVAL : nos_setfd(fd, argument);
        break;
    case F_GETFL:
        rc = nos_getflags(fd);
        if (rc >= 0)
            rc = (rc & 3) | (rc & NOS_O_APPEND ? O_APPEND : 0) |
                 (rc & NOS_O_NONBLOCK ? O_NONBLOCK : 0);
        break;
    case F_SETFL:
        rc = (argument & ~(O_ACCMODE | O_APPEND | O_NONBLOCK))
                 ? -NOS_ENOTSUP
                 : nos_setflags(fd, (argument & O_APPEND ? NOS_O_APPEND : 0) |
                                        (argument & O_NONBLOCK ? NOS_O_NONBLOCK : 0));
        break;
    default:
        rc = -NOS_ENOTSUP;
    }
    return (int)nos_libc_result(r, rc);
}
int __wrap_fcntl(int fd, int command, ...) {
    int argument = 0;
    if (command != F_GETFD && command != F_GETFL && command != F_GETOWN) {
        /* Unsupported native commands do not consume a potentially absent
         * variadic argument. SDK commands retain their pinned integer ABI. */
        int c = nos_libc_context(__getreent());
        if (fd < (int)NOS_FD_LIMIT && c == 1 && command != F_DUPFD && command != F_DUPFD_CLOEXEC &&
            command != F_SETFL && command != F_SETFD)
            return (int)nos_libc_result(__getreent(), -NOS_ENOTSUP);
        va_list args;
        va_start(args, command);
        argument = va_arg(args, int);
        va_end(args);
    }
    return __wrap__fcntl_r(__getreent(), fd, command, argument);
}
int __wrap__unlink_r(struct _reent *r, const char *path) {
    int c = nos_libc_context(r);
    return !c ? esp_vfs_unlink(r, path) : (int)nos_libc_result(r, c < 0 ? c : nos_unlink(path));
}
int __wrap__rename_r(struct _reent *r, const char *old, const char *next) {
    int c = nos_libc_context(r);
    return !c ? esp_vfs_rename(r, old, next)
              : (int)nos_libc_result(r, c < 0 ? c : nos_rename(old, next));
}
int __wrap__link_r(struct _reent *r, const char *old, const char *next) {
    int c = nos_libc_context(r);
    return !c ? esp_vfs_link(r, old, next)
              : (int)nos_libc_result(r, c < 0 ? c
                                              : nos_link_at(NOS_AT_FDCWD, old, NOS_AT_FDCWD, next));
}
#define REQUIRE_NATIVE()                                                                           \
    do {                                                                                           \
        int c = nos_libc_context(__getreent());                                                    \
        if (c != 1)                                                                                \
            return (int)nos_libc_result(__getreent(), c < 0 ? c : -NOS_ENOSYS);                    \
    } while (0)
int __wrap_mkdir(const char *path, mode_t mode) {
    REQUIRE_NATIVE();
    return (int)nos_libc_result(__getreent(), nos_mkdirat(NOS_AT_FDCWD, path, mode));
}
int __wrap_rmdir(const char *path) {
    REQUIRE_NATIVE();
    return (int)nos_libc_result(__getreent(), nos_rmdir(path));
}
int __wrap_chdir(const char *path) {
    REQUIRE_NATIVE();
    return (int)nos_libc_result(__getreent(), nos_chdir(path));
}
char *__wrap_getcwd(char *buffer, size_t size) {
    int c = nos_libc_context(__getreent());
    if (c != 1) {
        nos_libc_result(__getreent(), c < 0 ? c : -NOS_ENOSYS);
        return NULL;
    }
    int allocated = !buffer;
    if (!size && !allocated) {
        errno = EINVAL;
        return NULL;
    }
    if (allocated && !(buffer = malloc(size ? size : (size = NOS_PATH_MAX))))
        return NULL;
    if (nos_libc_result(__getreent(), nos_getcwd(buffer, size)) < 0) {
        if (allocated)
            free(buffer);
        return NULL;
    }
    return buffer;
}
int __wrap_dup(int fd) {
    REQUIRE_NATIVE();
    return (int)nos_libc_result(__getreent(), nos_dup(fd));
}
int __wrap_dup2(int fd, int target) {
    REQUIRE_NATIVE();
    return (int)nos_libc_result(__getreent(), nos_dup2(fd, target));
}
int __wrap_pipe(int fds[2]) {
    REQUIRE_NATIVE();
    return (int)nos_libc_result(__getreent(), nos_pipe(fds));
}
int __wrap_ftruncate(int fd, off_t size) {
    int c = nos_libc_context(__getreent());
    if (!c || fd >= (int)NOS_FD_LIMIT)
        return __real_ftruncate(fd, size);
    return (int)nos_libc_result(__getreent(), c < 0 ? c : nos_ftruncate(fd, size));
}
int __wrap_truncate(const char *path, off_t size) {
    REQUIRE_NATIVE();
    return (int)nos_libc_result(__getreent(), nos_truncate(path, size));
}
int __wrap_fsync(int fd) {
    int c = nos_libc_context(__getreent());
    if (!c || fd >= (int)NOS_FD_LIMIT)
        return __real_fsync(fd);
    if (c < 0)
        return (int)nos_libc_result(__getreent(), c);
    struct nos_stat info;
    int rc = nos_stat_fd(fd, &info);
    if (!rc && !S_ISREG(info.mode) && !S_ISDIR(info.mode))
        rc = -NOS_EINVAL;
    return (int)nos_libc_result(__getreent(), rc ? rc : nos_sync());
}
ssize_t __wrap_pread(int fd, void *buffer, size_t size, off_t offset) {
    int c = nos_libc_context(__getreent());
    if (!c || fd >= (int)NOS_FD_LIMIT)
        return __real_pread(fd, buffer, size, offset);
    return nos_libc_result(__getreent(), c < 0 ? c : nos_pread(fd, buffer, size, offset));
}
ssize_t __wrap_pwrite(int fd, const void *buffer, size_t size, off_t offset) {
    int c = nos_libc_context(__getreent());
    if (!c || fd >= (int)NOS_FD_LIMIT)
        return __real_pwrite(fd, buffer, size, offset);
    return nos_libc_result(__getreent(), c < 0 ? c : nos_pwrite(fd, buffer, size, offset));
}
