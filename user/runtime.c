/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
#include "user.h"
#ifdef NOS_NEWLIB
#include <reent.h>
#include <string.h>
#endif
intptr_t nos_syscall(uintptr_t number, uintptr_t a, uintptr_t b, uintptr_t c)
{
#if defined(NOS_ARM64)
    register uintptr_t r0 __asm__("x0") = a, r1 __asm__("x1") = b, r2 __asm__("x2") = c;
    register uintptr_t nr __asm__("x8") = number;
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(nr) : "memory", "cc");
#elif defined(NOS_RISCV)
    register uintptr_t r0 __asm__("a0") = a, r1 __asm__("a1") = b, r2 __asm__("a2") = c;
    register uintptr_t nr __asm__("a7") = number;
    __asm__ volatile("ecall" : "+r"(r0) : "r"(r1), "r"(r2), "r"(nr) : "memory");
#else
    register uintptr_t r0 __asm__("r0") = a, r1 __asm__("r1") = b, r2 __asm__("r2") = c;
#ifdef NOS_ARM_M
    register uintptr_t nr __asm__("r12") = number;
#else
    register uintptr_t nr __asm__("r7") = number;
#endif
    __asm__ volatile("svc #0" : "+r"(r0) : "r"(r1), "r"(r2), "r"(nr) : "memory", "cc");
#endif
    return (intptr_t)r0;
}
void *u_memcpy(void *dst, const void *src, size_t n)
{ unsigned char *d = dst; const unsigned char *s = src; while (n--) *d++ = *s++; return dst; }
void *u_memset(void *dst, int value, size_t n)
{ unsigned char *d = dst; while (n--) *d++ = (unsigned char)value; return dst; }
size_t u_strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
int u_streq(const char *a, const char *b)
{ while (*a && *a == *b) { a++; b++; } return *a == *b; }
#if !defined(NOS_NEWLIB) && !defined(NOS_MUSL)
void *memcpy(void *d, const void *s, size_t n) { return u_memcpy(d, s, n); }
void *memset(void *d, int value, size_t n) { return u_memset(d, value, n); }
#if defined(NOS_ARM) || defined(NOS_ARM_M)
void __aeabi_memcpy(void *d, const void *s, size_t n) { u_memcpy(d, s, n); }
void __aeabi_memcpy4(void *d, const void *s, size_t n) { u_memcpy(d, s, n); }
void __aeabi_memclr(void *d, size_t n) { u_memset(d, 0, n); }
void __aeabi_memclr4(void *d, size_t n) { u_memset(d, 0, n); }
#endif
#endif
int nos_user_check(const void *pointer, size_t size, unsigned protection)
{
    if (protection == (NOS_PROT_READ | NOS_PROT_WRITE)) {
        int rc = (int)nos_syscall(NOS_SYS_USER_CHECK, (uintptr_t)pointer, size, NOS_PROT_READ);
        return rc ? rc : (int)nos_syscall(NOS_SYS_USER_CHECK, (uintptr_t)pointer, size, NOS_PROT_WRITE);
    }
    return (int)nos_syscall(NOS_SYS_USER_CHECK, (uintptr_t)pointer, size, protection);
}
int nos_user_copy(void *destination, const void *source, size_t size)
{ return (int)nos_syscall(NOS_SYS_USER_COPY, (uintptr_t)destination, (uintptr_t)source, size); }
void nos_puts(const char *text)
{
    size_t remaining = u_strlen(text);
    while (remaining) {
        size_t count = remaining < 1024 ? remaining : 1024;
        intptr_t rc = nos_syscall(NOS_SYS_WRITE, (uintptr_t)text, count, 0);
        if (rc <= 0) break;
        text += rc; remaining -= (size_t)rc;
    }
}
__attribute__((weak)) void nos_libc_task_cleanup(void) { }
void nos_process_exit(int status)
{
    nos_libc_task_cleanup();
    nos_syscall(NOS_SYS_EXIT, (uintptr_t)status, 0, 0);
    for (;;) { }
}
void nos_idle(uintptr_t unused) { (void)unused; for (;;) nos_syscall(NOS_SYS_YIELD, 0, 0, 0); }
void nos_process_start(uintptr_t entry, uintptr_t argument)
{
#ifdef NOS_NEWLIB
    extern void nos_newlib_init(void);
    nos_newlib_init();
    _Static_assert(sizeof(struct _reent) <= NOS_PAGE_SIZE, "Newlib thread storage is too large");
    struct _reent local_reent;
    struct _reent *reent = &local_reent;
    _REENT_INIT_PTR(reent);
#ifdef NOS_NEWLIB_DYNAMIC
    extern int nos_newlib_bind_reent(struct _reent *);
    if (nos_newlib_bind_reent(reent))
#else
    if (nos_syscall(NOS_SYS_TLS_BIND, (uintptr_t)&_impure_ptr, (uintptr_t)reent, 0))
#endif
        nos_syscall(NOS_SYS_EXIT, 1, 0, 0);
#endif
#ifdef NOS_MUSL
    static uintptr_t environment[] = { 0, 6, NOS_PAGE_SIZE, 0, 0 };
    extern void __init_libc(char **environment, char *name);
    __init_libc((char **)environment, "nos");
#endif
    ((void (*)(uintptr_t))entry)(argument);
    nos_process_exit(0);
}
intptr_t nos_rpc(struct nos_message *m)
{
    intptr_t rc = nos_syscall(NOS_SYS_CALL, 0, (uintptr_t)m, (uintptr_t)m);
    return rc < 0 ? rc : m->value;
}
static int copy_path(char *destination, const char *path, size_t *length)
{
    if (!path) return -NOS_EFAULT;
    for (size_t i = 0; i < NOS_MSG_DATA; i++) {
        int rc = nos_user_copy(destination + i, path + i, 1);
        if (rc) return rc;
        if (!destination[i]) { *length = i + 1; return 0; }
    }
    return -NOS_ENAMETOOLONG;
}
static int path_request(unsigned op, const char *path, unsigned flags)
{
    struct nos_message m = { .op = op, .arg = flags };
    int rc = copy_path(m.data, path, &m.len);
    return rc ? rc : (int)nos_rpc(&m);
}
int nos_open(const char *p, unsigned f) { return path_request(NOS_VFS_OPEN, p, f); }
int nos_mkdir(const char *p) { return path_request(NOS_VFS_MKDIR, p, 0); }
int nos_unlink(const char *p) { return path_request(NOS_VFS_UNLINK, p, 0); }
int nos_close(int fd)
{
    struct nos_message m = { .op = NOS_VFS_CLOSE, .arg = fd };
    return (int)nos_rpc(&m);
}
static intptr_t read_mode(int fd, void *buffer, size_t count, unsigned mode)
{
    int check = nos_user_check(buffer, count, NOS_PROT_WRITE);
    if (check) return check;
    size_t done = 0;
    do {
        struct nos_message m = { .op = NOS_VFS_READ, .arg = fd,
            .value = mode || done ? NOS_IO_DONTWAIT : 0 };
        size_t requested = count - done < NOS_MSG_DATA ? count - done : NOS_MSG_DATA;
        m.len = requested;
        intptr_t n = nos_rpc(&m);
        if (n < 0) return done ? (intptr_t)done : n;
        if (!n) break;
        if ((size_t)n > requested || (size_t)n > m.len || m.len > NOS_MSG_DATA)
            return done ? (intptr_t)done : -NOS_EIO;
        int rc = nos_user_copy((char *)buffer + done, m.data, (size_t)n);
        if (rc) return done ? (intptr_t)done : rc;
        done += (size_t)n;
        if ((size_t)n < requested) break;
    } while (done < count);
    return (intptr_t)done;
}
intptr_t nos_read(int fd, void *buffer, size_t count) { return read_mode(fd, buffer, count, 0); }
intptr_t nos_read_nowait(int fd, void *buffer, size_t count) { return read_mode(fd, buffer, count, 1); }
static intptr_t write_mode(int fd, const void *buffer, size_t count, unsigned mode)
{
    int check = nos_user_check(buffer, count, NOS_PROT_READ);
    if (check) return check;
    size_t done = 0;
    do {
        struct nos_message m = { .op = NOS_VFS_WRITE, .arg = fd,
            .value = mode || done ? NOS_IO_DONTWAIT : 0 };
        size_t requested = count - done < NOS_MSG_DATA ? count - done : NOS_MSG_DATA;
        m.len = requested;
        int rc = nos_user_copy(m.data, (const char *)buffer + done, requested);
        if (rc) return done ? (intptr_t)done : rc;
        intptr_t n = nos_rpc(&m);
        if (n < 0) return done ? (intptr_t)done : n;
        if (!n) break;
        if ((size_t)n > requested) return done ? (intptr_t)done : -NOS_EIO;
        done += (size_t)n;
        if ((size_t)n < requested) break;
    } while (done < count);
    return (intptr_t)done;
}
intptr_t nos_write(int fd, const void *buffer, size_t count) { return write_mode(fd, buffer, count, 0); }
intptr_t nos_write_nowait(int fd, const void *buffer, size_t count) { return write_mode(fd, buffer, count, 1); }
intptr_t nos_seek(int fd, intptr_t offset, int whence)
{
    struct nos_message m = { .op = NOS_VFS_SEEK, .arg = fd, .value = offset, .len = (size_t)whence };
    return nos_rpc(&m);
}
int nos_stat_fd(int fd, struct nos_stat *st)
{
    struct nos_message m = { .op = NOS_VFS_STAT, .arg = fd };
    intptr_t rc = nos_rpc(&m);
    if (!rc) rc = nos_user_copy(st, m.data, sizeof(*st));
    return (int)rc;
}

int nos_rmdir(const char *path) { return path_request(NOS_VFS_RMDIR, path, 0); }
int nos_mount(const char *path, unsigned backend) { return path_request(NOS_VFS_MOUNT, path, backend); }
int nos_rename(const char *old_path, const char *new_path)
{
    struct nos_message m = { .op = NOS_VFS_RENAME };
    char second[NOS_MSG_DATA];
    size_t first_length, second_length;
    int rc = copy_path(m.data, old_path, &first_length);
    if (rc) return rc;
    rc = copy_path(second, new_path, &second_length);
    if (rc) return rc;
    if (first_length + second_length > sizeof(m.data)) return -NOS_ENAMETOOLONG;
    u_memcpy(m.data + first_length, second, second_length);
    m.len = first_length + second_length;
    return (int)nos_rpc(&m);
}
int nos_readdir(int fd, size_t index, char *name, size_t size)
{
    int rc = nos_user_check(name, size, NOS_PROT_WRITE);
    if (rc) return rc;
    struct nos_message m = { .op = NOS_VFS_READDIR, .arg = fd, .value = (intptr_t)index };
    intptr_t result = nos_rpc(&m);
    if (result <= 0) return (int)result;
    if (!m.len || m.len > sizeof(m.data) || m.data[m.len - 1]) return -NOS_EIO;
    if (m.len > size) return -NOS_ENAMETOOLONG;
    rc = nos_user_copy(name, m.data, m.len);
    return rc ? rc : 1;
}
int nos_dup2(int fd, int target)
{
    struct nos_message m = { .op = NOS_VFS_DUP, .arg = fd, .value = target };
    return (int)nos_rpc(&m);
}
int nos_dup_min(int fd, unsigned minimum)
{
    struct nos_message m = { .op = NOS_VFS_DUP, .arg = fd, .value = -1, .len = minimum };
    return (int)nos_rpc(&m);
}
int nos_dup(int fd) { return nos_dup_min(fd, 0); }
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
int nos_pipe(int descriptors[2]) { return nos_pipe2(descriptors, 0); }
int nos_pipe2(int descriptors[2], unsigned flags)
{
    int rc = nos_user_check(descriptors, 2 * sizeof(int), NOS_PROT_WRITE);
    if (rc) return rc;
    struct nos_message m = { .op = NOS_VFS_PIPE, .arg = flags };
    intptr_t result = nos_rpc(&m);
    if (result) return (int)result;
    if (m.len != 2 * sizeof(int)) return -NOS_EIO;
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
        if (rc != -NOS_EAGAIN) return rc;
        int flags = nos_getflags(fd);
        if (flags < 0) return flags;
        if (flags & NOS_O_NONBLOCK) return -NOS_EAGAIN;
        rc = nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
        if (rc < 0) return rc;
    }
}
intptr_t nos_posix_write(int fd, const void *buffer, size_t count)
{
    for (;;) {
        intptr_t rc = nos_write(fd, buffer, count);
        if (rc != -NOS_EAGAIN) return rc;
        int flags = nos_getflags(fd);
        if (flags < 0) return flags;
        if (flags & NOS_O_NONBLOCK) return -NOS_EAGAIN;
        rc = nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
        if (rc < 0) return rc;
    }
}

void nos_time_from_ticks(uint64_t ticks, struct nos_time_value *value)
{
    value->seconds = (int64_t)(ticks / 100);
    value->nanoseconds = (uint32_t)(ticks % 100) * 10000000u;
}
int nos_clock_snapshot(struct nos_time_value *value)
{
    uint64_t ticks;
    int rc = (int)nos_syscall(NOS_SYS_TICKS64, (uintptr_t)&ticks, 0, 0);
    if (!rc) nos_time_from_ticks(ticks, value);
    return rc;
}
int nos_sleep_ticks64(uint64_t ticks, uint64_t *remaining)
{
    uint64_t start, now;
    int rc = (int)nos_syscall(NOS_SYS_TICKS64, (uintptr_t)&start, 0, 0);
    if (rc) return rc;
    if (ticks > UINT64_MAX - start) return -NOS_EOVERFLOW;
    now = start;
    while (now - start < ticks) {
        uint64_t left = ticks - (now - start);
        rc = (int)nos_syscall(NOS_SYS_SLEEP, left > 10000 ? 10000 : (uintptr_t)left, 0, 0);
        int clock_rc = (int)nos_syscall(NOS_SYS_TICKS64, (uintptr_t)&now, 0, 0);
        if (clock_rc) return clock_rc;
        if (rc) {
            if (remaining) *remaining = now - start >= ticks ? 0 : ticks - (now - start);
            return rc;
        }
    }
    if (remaining) *remaining = 0;
    return 0;
}
