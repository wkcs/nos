/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * Standard C Library (Newlib / Picolibc) low-level port for NOS.
 *
 * Bridges standard C library system calls (_read, _write, _open, _close,
 * _lseek, _fstat, _isatty, _sbrk, __malloc_lock/__malloc_unlock, _getpid,
 * _kill, _exit, _gettimeofday, _times, _unlink, _link) to NOS kernel
 * console, VFS, memory allocator, and task subsystems.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/times.h>
#include <sys/reent.h>
#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <limits.h>
#include <string.h>

#include <kernel/kernel.h>
#include <kernel/console.h>
#include <kernel/irq.h>
#include <kernel/mm.h>
#include <kernel/sched.h>
#include <kernel/spinlock.h>
#include <kernel/task.h>
#include <kernel/timer.h>
#include <lib/vsprintf.h>
#include <fs/fs.h>
#include <fs/vfs.h>

#define NOS_LIBC_SBRK_HEAP_SIZE     2048U

/* Newlib internal <sys/_default_fcntl.h> flag bits */
#define NEWLIB_FAPPEND              0x0008
#define NEWLIB_FCREAT               0x0200
#define NEWLIB_FTRUNC               0x0400
#define NEWLIB_FEXCL                0x0800
#define NEWLIB_FNONBLOCK            0x4000

static uint8_t *g_libc_sbrk_arena;
static size_t  g_libc_sbrk_offset;

/* Recursive IRQ-safe lock for Newlib/Picolibc heap operations */
static struct task_struct *g_malloc_lock_owner;
static uint32_t            g_malloc_lock_depth;
static addr_t              g_malloc_irq_state;

void __malloc_lock(struct _reent *r)
{
    (void)r;
    addr_t flags = disable_irq_save();
    struct task_struct *curr = kernel_running ? task_current() : NULL;

    if (g_malloc_lock_depth > 0 && g_malloc_lock_owner == curr) {
        g_malloc_lock_depth++;
        enable_irq_save(flags);
        return;
    }

    g_malloc_irq_state = flags;
    g_malloc_lock_owner = curr;
    g_malloc_lock_depth = 1;
}

void __malloc_unlock(struct _reent *r)
{
    (void)r;
    if (g_malloc_lock_depth == 0) {
        return;
    }
    g_malloc_lock_depth--;
    if (g_malloc_lock_depth == 0) {
        addr_t flags = g_malloc_irq_state;
        g_malloc_lock_owner = NULL;
        enable_irq_save(flags);
    }
}

#if !defined(__HOST_TEST__)
void *malloc(size_t size)
{
    return kmalloc((u32)size, GFP_KERNEL);
}

void *calloc(size_t nitems, size_t size)
{
    if (nitems != 0 && size > (SIZE_MAX / nitems)) {
        return NULL;
    }
    return kzalloc((u32)(nitems * size), GFP_KERNEL);
}

void *realloc(void *ptr, size_t size)
{
    return krealloc(ptr, (u32)size, GFP_KERNEL);
}

void free(void *ptr)
{
    if (ptr != NULL) {
        kfree(ptr);
    }
}

void *_malloc_r(struct _reent *r, size_t size)
{
    (void)r;
    return malloc(size);
}

void *_calloc_r(struct _reent *r, size_t nitems, size_t size)
{
    (void)r;
    return calloc(nitems, size);
}

void *_realloc_r(struct _reent *r, void *ptr, size_t size)
{
    (void)r;
    return realloc(ptr, size);
}

void _free_r(struct _reent *r, void *ptr)
{
    (void)r;
    free(ptr);
}
#endif

int _vsnprintf_r(struct _reent *r, char *buf, size_t size, const char *fmt, va_list args)
{
    (void)r;
    return vsnprintf(buf, size, fmt, args);
}

int _snprintf_r(struct _reent *r, char *buf, size_t size, const char *fmt, ...)
{
    int n;
    va_list args;

    (void)r;
    va_start(args, fmt);
    n = vsnprintf(buf, size, fmt, args);
    va_end(args);
    return n;
}

int _vsprintf_r(struct _reent *r, char *buf, const char *format, va_list arg_ptr)
{
    (void)r;
    return vsprintf(buf, format, arg_ptr);
}

int _sprintf_r(struct _reent *r, char *buf, const char *format, ...)
{
    int n;
    va_list arg_ptr;

    (void)r;
    va_start(arg_ptr, format);
    n = vsprintf(buf, format, arg_ptr);
    va_end(arg_ptr);
    return n;
}

void *_sbrk_r(struct _reent *r, ptrdiff_t incr)
{
    void *prev_brk;

    __malloc_lock(r);
    if (g_libc_sbrk_arena == NULL) {
        g_libc_sbrk_arena = (uint8_t *)kmalloc(NOS_LIBC_SBRK_HEAP_SIZE, GFP_KERNEL);
        if (g_libc_sbrk_arena == NULL) {
            __malloc_unlock(r);
            if (r) {
                r->_errno = ENOMEM;
            }
            errno = ENOMEM;
            return (void *)-1;
        }
    }
    if (incr < 0 && (size_t)(-incr) > g_libc_sbrk_offset) {
        __malloc_unlock(r);
        if (r) {
            r->_errno = ENOMEM;
        }
        errno = ENOMEM;
        return (void *)-1;
    }
    if (incr > 0 && (g_libc_sbrk_offset + (size_t)incr) > NOS_LIBC_SBRK_HEAP_SIZE) {
        __malloc_unlock(r);
        if (r) {
            r->_errno = ENOMEM;
        }
        errno = ENOMEM;
        return (void *)-1;
    }

    prev_brk = &g_libc_sbrk_arena[g_libc_sbrk_offset];
    g_libc_sbrk_offset = (size_t)((ptrdiff_t)g_libc_sbrk_offset + incr);
    __malloc_unlock(r);
    return prev_brk;
}

void *_sbrk(ptrdiff_t incr)
{
    return _sbrk_r(_REENT, incr);
}

static int translate_newlib_open_flags(int flags)
{
    int nos_flags = flags & O_ACCMODE;

    if (flags & (NEWLIB_FCREAT | O_CREAT)) {
        nos_flags |= O_CREAT;
    }
    if (flags & (NEWLIB_FTRUNC | O_TRUNC)) {
        nos_flags |= O_TRUNC;
    }
    if (flags & (NEWLIB_FEXCL | O_EXCL)) {
        nos_flags |= O_EXCL;
    }
    if (flags & (NEWLIB_FAPPEND | O_APPEND)) {
        nos_flags |= O_APPEND;
    }
    if (flags & (NEWLIB_FNONBLOCK | O_NONBLOCK)) {
        nos_flags |= O_NONBLOCK;
    }
    return nos_flags;
}

_ssize_t _write_r(struct _reent *r, int fd, const void *buf, size_t cnt)
{
    if (buf == NULL) {
        if (r) {
            r->_errno = EINVAL;
        }
        errno = EINVAL;
        return -1;
    }
    if (cnt == 0) {
        return 0;
    }

#if defined(CONFIG_VFS) || defined(CONFIG_FS)
    if (fd >= 0) {
        ssize_t ret = nos_write(fd, buf, cnt);
        if (ret >= 0) {
            return (_ssize_t)ret;
        }
        /* 防御性回退：若 nos_write 返回 -EBADF 且 fd 为 1 或 2，回退到 console_write */
        if (ret == -EBADF && (fd == 1 || fd == 2)) {
            int rc = console_write((const char *)buf, cnt > INT_MAX ? INT_MAX : (int)cnt);
            if (rc < 0) {
                if (r != NULL) {
                    r->_errno = -rc;
                }
                errno = -rc;
                return -1;
            }
            return (_ssize_t)rc;
        }
        if (r) {
            r->_errno = (int)(-ret);
        }
        errno = (int)(-ret);
        return -1;
    }
#else
    if (fd == 1 || fd == 2) {
        int rc = console_write((const char *)buf, cnt > INT_MAX ? INT_MAX : (int)cnt);
        if (rc < 0) {
            if (r != NULL) {
                r->_errno = -rc;
            }
            errno = -rc;
            return -1;
        }
        return (_ssize_t)rc;
    }
#endif

    if (r) {
        r->_errno = EBADF;
    }
    errno = EBADF;
    return -1;
}

_ssize_t _write(int fd, const void *buf, size_t cnt)
{
    return _write_r(_REENT, fd, buf, cnt);
}

_ssize_t _read_r(struct _reent *r, int fd, void *buf, size_t cnt)
{
    if (buf == NULL) {
        if (r) {
            r->_errno = EINVAL;
        }
        errno = EINVAL;
        return -1;
    }
    if (cnt == 0) {
        return 0;
    }

#if defined(CONFIG_VFS) || defined(CONFIG_FS)
    if (fd >= 0) {
        ssize_t ret = nos_read(fd, buf, cnt);
        if (ret >= 0) {
            return (_ssize_t)ret;
        }
        /* 防御性回退：若 nos_read 返回 -EBADF 且 fd 为 0，回退到 console_getc */
        if (ret == -EBADF && fd == 0) {
            char *dst = (char *)buf;
            int ch = console_getc();
            if (ch < 0) {
                if (r != NULL) {
                    r->_errno = -ch;
                }
                errno = -ch;
                return -1;
            }
            dst[0] = (char)ch;
            return 1;
        }
        if (r) {
            r->_errno = (int)(-ret);
        }
        errno = (int)(-ret);
        return -1;
    }
#else
    if (fd == 0) {
        char *dst = (char *)buf;
        int ch = console_getc();
        if (ch < 0) {
            if (r != NULL) {
                r->_errno = -ch;
            }
            errno = -ch;
            return -1;
        }
        dst[0] = (char)ch;
        return 1;
    }
#endif

    if (r) {
        r->_errno = EBADF;
    }
    errno = EBADF;
    return -1;
}

_ssize_t _read(int fd, void *buf, size_t cnt)
{
    return _read_r(_REENT, fd, buf, cnt);
}

int _open_r(struct _reent *r, const char *path, int flags, int mode)
{
#if defined(CONFIG_VFS) || defined(CONFIG_FS)
    int nos_flags = translate_newlib_open_flags(flags);
    int vfs_fd = nos_open(path, nos_flags, mode);
    if (vfs_fd < 0) {
        if (r) {
            r->_errno = -vfs_fd;
        }
        errno = -vfs_fd;
        return -1;
    }
    return vfs_fd;
#else
    (void)path;
    (void)flags;
    (void)mode;
    if (r) {
        r->_errno = ENOSYS;
    }
    errno = ENOSYS;
    return -1;
#endif
}

int _open(const char *path, int flags, ...)
{
    return _open_r(_REENT, path, flags, 0644);
}

int _close_r(struct _reent *r, int fd)
{
#if defined(CONFIG_VFS) || defined(CONFIG_FS)
    if (fd >= 0) {
        int rc = nos_close(fd);
        if (rc < 0) {
            if (r) {
                r->_errno = -rc;
            }
            errno = -rc;
            return -1;
        }
        return 0;
    }
#endif
    if (r) {
        r->_errno = EBADF;
    }
    errno = EBADF;
    return -1;
}

int _close(int fd)
{
    return _close_r(_REENT, fd);
}

_off_t _lseek_r(struct _reent *r, int fd, _off_t offset, int whence)
{
#if defined(CONFIG_VFS) || defined(CONFIG_FS)
    if (fd >= 0) {
        loff_t pos = nos_lseek(fd, (loff_t)offset, whence);
        if (pos < 0) {
            if (r) {
                r->_errno = (int)(-pos);
            }
            errno = (int)(-pos);
            return (_off_t)-1;
        }
        return (_off_t)pos;
    }
#else
    (void)fd;
    (void)offset;
    (void)whence;
#endif
    if (r) {
        r->_errno = ESPIPE;
    }
    errno = ESPIPE;
    return (_off_t)-1;
}

_off_t _lseek(int fd, _off_t offset, int whence)
{
    return _lseek_r(_REENT, fd, offset, whence);
}

int _fstat_r(struct _reent *r, int fd, struct stat *st)
{
    if (st == NULL) {
        if (r) {
            r->_errno = EINVAL;
        }
        errno = EINVAL;
        return -1;
    }
    if (fd < 0) {
        if (r) {
            r->_errno = EBADF;
        }
        errno = EBADF;
        return -1;
    }
    memset(st, 0, sizeof(*st));
#if defined(CONFIG_VFS) || defined(CONFIG_FS)
    struct file *file = vfs_fd_get(fd);
    if (file != NULL) {
        if (file->f_inode != NULL) {
            st->st_mode = (mode_t)file->f_inode->i_mode;
            st->st_size = (_off_t)file->f_inode->i_size;
        } else {
            st->st_mode = S_IFREG | 0644;
        }
        vfs_fd_put(file);
        return 0;
    }
#endif
    /* 防御性回退：早期控制台或无 fd 表任务的标准流 */
    if (fd < 3) {
        st->st_mode = S_IFCHR | 0666;
        return 0;
    }
    if (r) {
        r->_errno = EBADF;
    }
    errno = EBADF;
    return -1;
}

int _fstat(int fd, struct stat *st)
{
    return _fstat_r(_REENT, fd, st);
}

int _stat_r(struct _reent *r, const char *path, struct stat *st)
{
    (void)path;
    if (st == NULL) {
        if (r) {
            r->_errno = EINVAL;
        }
        errno = EINVAL;
        return -1;
    }
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFREG | 0644;
    return 0;
}

int _stat(const char *path, struct stat *st)
{
    return _stat_r(_REENT, path, st);
}

int _isatty_r(struct _reent *r, int fd)
{
    if (fd < 0) {
        if (r) {
            r->_errno = EBADF;
        }
        errno = EBADF;
        return 0;
    }
#if defined(CONFIG_VFS) || defined(CONFIG_FS)
    struct file *file = vfs_fd_get(fd);
    if (file != NULL) {
        int is_tty = (file->f_inode != NULL && S_ISCHR(file->f_inode->i_mode)) ? 1 : 0;
        vfs_fd_put(file);
        return is_tty;
    }
#endif
    /* 防御性回退：早期未打开 fd 表时 0/1/2 视为 tty */
    if (fd < 3) {
        return 1;
    }
    if (r) {
        r->_errno = EBADF;
    }
    errno = EBADF;
    return 0;
}

int _isatty(int fd)
{
    return _isatty_r(_REENT, fd);
}

int _unlink_r(struct _reent *r, const char *path)
{
    (void)path;
    if (r) {
        r->_errno = ENOSYS;
    }
    errno = ENOSYS;
    return -1;
}

int _unlink(const char *path)
{
    return _unlink_r(_REENT, path);
}

int _link_r(struct _reent *r, const char *oldpath, const char *newpath)
{
    (void)oldpath;
    (void)newpath;
    if (r) {
        r->_errno = ENOSYS;
    }
    errno = ENOSYS;
    return -1;
}

int _link(const char *oldpath, const char *newpath)
{
    return _link_r(_REENT, oldpath, newpath);
}

int _getpid_r(struct _reent *r)
{
    (void)r;
    struct task_struct *curr = kernel_running ? task_current() : NULL;
    return curr ? (int)curr->pid : 1;
}

int _getpid(void)
{
    return _getpid_r(_REENT);
}

int _kill_r(struct _reent *r, int pid, int sig)
{
    (void)pid;
    (void)sig;
    if (r) {
        r->_errno = EINVAL;
    }
    errno = EINVAL;
    return -1;
}

int _kill(int pid, int sig)
{
    return _kill_r(_REENT, pid, sig);
}

void _exit(int status)
{
    (void)status;
    pr_err("task exited with status %d\n", status);
    while (1) {
        ;
    }
}

int _gettimeofday_r(struct _reent *r, struct timeval *tv, void *tz)
{
    (void)r;
    (void)tz;
    if (tv != NULL) {
        uint32_t tick_ms = CONFIG_SYS_TICK_MS ? CONFIG_SYS_TICK_MS : 10U;
        uint64_t ms = sys_heartbeat_time * tick_ms;
        tv->tv_sec = (time_t)(ms / 1000ULL);
        tv->tv_usec = (suseconds_t)((ms % 1000ULL) * 1000ULL);
    }
    return 0;
}

int _gettimeofday(struct timeval *tv, void *tz)
{
    return _gettimeofday_r(_REENT, tv, tz);
}

clock_t _times_r(struct _reent *r, struct tms *buf)
{
    (void)r;
    clock_t now = (clock_t)sys_heartbeat_time;
    if (buf != NULL) {
        buf->tms_utime = now;
        buf->tms_stime = 0;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
    }
    return now;
}

clock_t _times(struct tms *buf)
{
    return _times_r(_REENT, buf);
}
