/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_ABI_H
#define NOS_ABI_H
#include <stdint.h>
#include <stddef.h>
#define NOS_PAGE_SIZE 4096u
#define NOS_MSG_DATA 64u
#if defined(NOS_MMU) || defined(NOS_RISCV32)
#define NOS_TASKS 12u
#else
#define NOS_TASKS 6u
#endif
#define NOS_ABI_VERSION 1u
#define NOS_MUTEXES 32u
#define NOS_ENDPOINTS 8u
#define NOS_CPUS 4u
#ifndef NOS_SMP
#define NOS_SMP 1u
#endif
#define NOS_IDLE_FIRST (NOS_TASKS - NOS_SMP)
#ifdef NOS_MMU
#define NOS_VM_REGIONS 16u
#else
#define NOS_VM_REGIONS 4u
#endif
static inline int nos_pointer_is_error(intptr_t value) { return (uintptr_t)value >= (uintptr_t)-4095; }
#define NOS_ENDPOINT_BASE 0x40000000u
#define NOS_MUTEX_HANDLE_MAX 0x1fffffffu
#define NOS_RIGHT_CALL 1u
#define NOS_RIGHT_RECV 2u
#define NOS_RIGHT_GRANT 4u
#define NOS_RIGHT_MUTEX 8u
#define NOS_ETIMEDOUT 110
#define NOS_EINTR 4
#define NOS_EOVERFLOW 75
#define NOS_EPERM 1
#define NOS_ENOENT 2
#define NOS_ESRCH 3
#define NOS_EIO 5
#define NOS_EBADF 9
#define NOS_EAGAIN 11
#define NOS_ENOMEM 12
#define NOS_EACCES 13
#define NOS_EFAULT 14
#define NOS_EBUSY 16
#define NOS_EEXIST 17
#define NOS_ENOTDIR 20
#define NOS_EISDIR 21
#define NOS_EINVAL 22
#define NOS_EMFILE 24
#define NOS_ENOSPC 28
#define NOS_EPIPE 32
#define NOS_EDEADLK 35
#define NOS_ENAMETOOLONG 36
#define NOS_ENOSYS 38
#define NOS_ENOTEMPTY 39
#define NOS_ENOTSUP 95
#define NOS_PROT_READ 1u
#define NOS_PROT_WRITE 2u
#define NOS_PROT_EXEC 4u
#define NOS_O_RDONLY 0u
#define NOS_O_WRONLY 1u
#define NOS_O_RDWR 2u
#define NOS_O_CREAT 0x40u
#define NOS_O_TRUNC 0x200u
#define NOS_O_APPEND 0x400u
#define NOS_O_NONBLOCK 0x800u
#define NOS_IO_DONTWAIT 1u
#define NOS_CAP_MMU 1u
#define NOS_CAP_USER 2u
#define NOS_CAP_PREEMPT 4u
#define NOS_CAP_IPC 8u
#define NOS_CAP_SPAWN 16u
#define NOS_CAP_DEVICE 32u
#define NOS_CAP_MPU 64u
#define NOS_CAP_SMP 128u
#define NOS_FEATURE_MASK (NOS_CAP_MMU | NOS_CAP_USER | NOS_CAP_PREEMPT | NOS_CAP_MPU | NOS_CAP_SMP)
#define NOS_PERMISSION_MASK (NOS_CAP_IPC | NOS_CAP_SPAWN | NOS_CAP_DEVICE)
#define NOS_IPC_PEER_EXIT (-1)
#define NOS_IPC_PEER_CANCEL (-2)
enum nos_syscall {
    NOS_SYS_WRITE = 0,
    NOS_SYS_GETPID = 1,
    NOS_SYS_YIELD = 2,
    NOS_SYS_EXIT = 3,
    NOS_SYS_SLEEP = 4,
    NOS_SYS_TICKS = 5,
    NOS_SYS_CAPS = 6,
    NOS_SYS_SPAWN = 7,
    NOS_SYS_WAIT = 8,
    NOS_SYS_CALL = 9,
    NOS_SYS_RECV = 10,
    NOS_SYS_REPLY = 11,
    NOS_SYS_MMAP = 12,
    NOS_SYS_MUNMAP = 13,
    NOS_SYS_MPROTECT = 14,
    NOS_SYS_BRK = 15,
    NOS_SYS_MUTEX_CREATE = 16,
    NOS_SYS_MUTEX_LOCK = 17,
    NOS_SYS_MUTEX_UNLOCK = 18,
    NOS_SYS_MUTEX_DESTROY = 19,
    NOS_SYS_TLS_STORAGE = 20,
    NOS_SYS_TLS_BIND = 21,
    NOS_SYS_PRIORITY = 22,
    NOS_SYS_FINISH = 23,
    NOS_SYS_USER_COPY = 24,
    NOS_SYS_USER_CHECK = 25,
    NOS_SYS_ABI_QUERY = 26,
    NOS_SYS_ENDPOINT_CREATE = 27,
    NOS_SYS_ENDPOINT_GRANT = 28,
    NOS_SYS_ENDPOINT_REVOKE = 29,
    NOS_SYS_CALL_EX = 30,
    NOS_SYS_IPC_CANCEL = 31,
    NOS_SYS_THREAD_CREATE = 32,
    NOS_SYS_SET_TID_ADDRESS = 33,
    NOS_SYS_FUTEX_WAIT = 34,
    NOS_SYS_FUTEX_WAKE = 35,
    NOS_SYS_TASK_CANCEL = 36,
    NOS_SYS_SERVICE_RESTART = 37,
    NOS_SYS_OBJECT_STATS = 38,
    NOS_SYS_DEVICE = 39,
    NOS_SYS_EXEC_SPAWN = 40,
    NOS_SYS_CPU_QUERY = 41,
    NOS_SYS_OBJECT_GRANT = 42,
    NOS_SYS_OBJECT_REVOKE = 43,
    NOS_SYS_GROUP_ID = 44,
    NOS_SYS_EXIT_GROUP = 45,
    NOS_SYS_TASK_AUTHORIZE = 46,
    NOS_SYS_IRQ_BIND = 47,
    NOS_SYS_IRQ_WAIT = 48,
    NOS_SYS_IRQ_ACK = 49,
    NOS_SYS_IRQ_RELEASE = 50,
    NOS_SYS_TLS_GET = 51,
    NOS_SYS_FEATURES = 52,
    NOS_SYS_PERMISSIONS = 53,
    NOS_SYS_ENDPOINT_CLOSE = 54,
    NOS_SYS_FUTEX_REQUEUE = 55,
    NOS_SYS_TICKS64 = 56,
};
enum nos_vfs_op {
    NOS_VFS_OPEN = 1, NOS_VFS_CLOSE, NOS_VFS_READ, NOS_VFS_WRITE,
    NOS_VFS_SEEK, NOS_VFS_STAT, NOS_VFS_MKDIR, NOS_VFS_UNLINK,
    NOS_VFS_RMDIR = 9, NOS_VFS_RENAME = 10, NOS_VFS_READDIR = 11,
    NOS_VFS_DUP = 12, NOS_VFS_PIPE = 13, NOS_VFS_MOUNT = 14, NOS_VFS_SYNC = 15,
    NOS_VFS_DEVICE_STATS = 16, NOS_VFS_GETFLAGS = 17, NOS_VFS_SETFLAGS = 18
};
struct nos_message {
    intptr_t sender, op, value, arg;
    size_t len;
    char data[NOS_MSG_DATA];
#if UINTPTR_MAX == UINT32_MAX
    uint32_t reserved_before_token;
#endif
    uint64_t token;
    uintptr_t process;
#if UINTPTR_MAX == UINT32_MAX
    uint32_t reserved_tail;
#endif
};
/* ABI uses native pointers; ABI_QUERY exposes version and word width. */
struct nos_abi_info { uint32_t version, word_bits, message_bytes, page_bytes; };
struct nos_call_args { uintptr_t endpoint, request, reply; uint64_t timeout_ticks; };
struct nos_futex_requeue { uintptr_t source, target; uint32_t wake_count, requeue_count, compare, flags; };
struct nos_thread_args { uintptr_t entry, arg, stack, stack_size, tls, clear_tid, parent_tid; };
struct nos_object_stats { uint32_t tasks, mutexes, endpoints, free_pages; };
struct nos_exec_request { const char *path; const char *const *argv; const char *const *envp; unsigned priority; };
struct nos_stat { size_t size; unsigned mode; };
intptr_t nos_syscall(uintptr_t number, uintptr_t a, uintptr_t b, uintptr_t c);
void nos_vfs_server(uintptr_t unused);
void nos_validation(uintptr_t unused);
void nos_process_start(uintptr_t entry, uintptr_t argument);
void nos_puts(const char *text);
void nos_check(int condition, const char *name);
intptr_t nos_rpc(struct nos_message *message);
int nos_open(const char *path, unsigned flags);
int nos_close(int fd);
intptr_t nos_read(int fd, void *buffer, size_t size);
intptr_t nos_write(int fd, const void *buffer, size_t size);
intptr_t nos_seek(int fd, intptr_t offset, int whence);
int nos_stat_fd(int fd, struct nos_stat *st);
int nos_mkdir(const char *path);
int nos_unlink(const char *path);
void *u_memcpy(void *dst, const void *src, size_t n);
void *u_memset(void *dst, int value, size_t n);
size_t u_strlen(const char *s);
int u_streq(const char *a, const char *b);
#endif
