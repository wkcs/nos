/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
#ifdef NOS_LINUX_COMPAT
#define MAPPING_TEST_LIMIT 256u
#else
#define MAPPING_TEST_LIMIT 64u
#endif
#if defined(NOS_NEWLIB) || defined(NOS_MUSL)
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#ifdef NOS_NEWLIB
#include <reent.h>
#endif
#ifdef NOS_NEWLIB
#define LIBC_TAG "newlib"
#else
#define LIBC_TAG "musl"
#endif
#endif
static unsigned failures;
static int mapping_valid(intptr_t address)
{ return address != 0 && (uintptr_t)address < (uintptr_t)-4095; }
void nos_vfs_regression(void);
void nos_libc_regression(void);
void nos_kernel_regression(void);
void nos_arch_regression(void);
void nos_elf_regression(void);
#ifdef NOS_POSIX_SUITE
void nos_posix_suite(void);
#elif defined(NOS_LINUX_COMPAT)
void nos_linux_regression(void);
#endif
void nos_persistence_regression(unsigned mode);
void nos_check(int condition, const char *name)
{
    nos_puts(condition ? "PASS " : "FAIL "); nos_puts(name); nos_puts("\n");
    if (!condition) failures++;
}
static void exit_task(int status)
{ nos_syscall(NOS_SYS_EXIT, (uintptr_t)status, 0, 0); for (;;) { } }
static void sleep_worker(uintptr_t unused)
{
    (void)unused;
    nos_syscall(NOS_SYS_SLEEP, 3, 0, 0);
    exit_task(23);
}
static void lock_worker(uintptr_t lock)
{
    intptr_t rc = nos_syscall(NOS_SYS_MUTEX_LOCK, lock, 0, 0);
    if (!rc) rc = nos_syscall(NOS_SYS_MUTEX_UNLOCK, lock, 0, 0);
    exit_task(rc ? 1 : 7);
}
static void fd_worker(uintptr_t fd)
{
    char byte;
    exit_task(nos_read((int)fd, &byte, 1) == -NOS_EBADF ? 9 : 1);
}
static void cleanup_worker(uintptr_t unused)
{
    (void)unused;
    int fd = nos_open("/tmp/exiting-client", NOS_O_CREAT | NOS_O_RDWR);
    exit_task(fd >= 3 ? 11 : 1);
}
#if defined(NOS_ARM) || defined(NOS_ARM64) || defined(NOS_RISCV)
extern void nos_fp_set(uint64_t pattern);
extern uint64_t nos_fp_get(void);
static void fp_worker(uintptr_t unused)
{
    (void)unused;
    nos_fp_set(0x4004000000000000ull);
    nos_syscall(NOS_SYS_SLEEP, 2, 0, 0);
    exit_task(nos_fp_get() == 0x4004000000000000ull ? 19 : 1);
}
#endif
#if defined(NOS_NEWLIB) || defined(NOS_MUSL)
static void libc_worker(uintptr_t unused)
{
    (void)unused;
    errno = 73;
    void *p = malloc(128);
    nos_syscall(NOS_SYS_SLEEP, 2, 0, 0);
    int ok = p && errno == 73;
    free(p);
    exit_task(ok ? 29 : 1);
}
#endif
#ifdef NOS_MMU
static void kernel_fault(uintptr_t address)
{ volatile uintptr_t value = *(volatile uintptr_t *)address; (void)value; exit_task(1); }
static void write_fault(uintptr_t unused)
{
    (void)unused;
    intptr_t p = nos_syscall(NOS_SYS_MMAP, NOS_PROT_READ | NOS_PROT_WRITE, 0, 0);
    if (!mapping_valid(p) || nos_syscall(NOS_SYS_MPROTECT, (uintptr_t)p, NOS_PROT_READ, 0)) exit_task(2);
    *(volatile unsigned char *)(uintptr_t)p = 1;
    exit_task(1);
}
static void execute_fault(uintptr_t unused)
{
    (void)unused;
    intptr_t p = nos_syscall(NOS_SYS_MMAP, 3, 0, 0);
    if (!mapping_valid(p)) exit_task(2);
    ((void (*)(void))(uintptr_t)p)();
    exit_task(1);
}
static void isolated_memory(uintptr_t address)
{
    intptr_t p = nos_syscall(NOS_SYS_MMAP, 3, 0, 0);
    if ((uintptr_t)p != address || *(unsigned char *)address != 0) exit_task(1);
    *(unsigned char *)address = 91;
    exit_task(17);
}
#endif
static intptr_t spawn(void (*entry)(uintptr_t), uintptr_t arg, unsigned priority)
{ return nos_syscall(NOS_SYS_SPAWN, (uintptr_t)entry, arg, priority); }
static int wait_status(intptr_t pid, intptr_t expected)
{
    intptr_t status = 0;
    return pid >= 0 && !nos_syscall(NOS_SYS_WAIT, (uintptr_t)pid, (uintptr_t)&status, 0) && status == expected;
}
static void resource_worker(uintptr_t unused)
{
    (void)unused;
    intptr_t mapping = nos_syscall(NOS_SYS_MMAP, NOS_PROT_READ | NOS_PROT_WRITE, NOS_PAGE_SIZE, 0);
    intptr_t lock = nos_syscall(NOS_SYS_MUTEX_CREATE, 0, 0, 0);
    int fd = nos_open("/tmp/resource-cycle", NOS_O_RDWR);
    int ok = mapping_valid(mapping) && lock >= 0 && fd >= 3;
    if (mapping_valid(mapping)) *(volatile unsigned char *)(uintptr_t)mapping = 0x51;
    if (lock >= 0) ok &= !nos_syscall(NOS_SYS_MUTEX_LOCK, (uintptr_t)lock, 0, 0);
#ifndef NOS_MMU
    /* Shared process-group locks outlive an individual MCU task by contract. */
    if (lock >= 0) {
        ok &= !nos_syscall(NOS_SYS_MUTEX_UNLOCK, (uintptr_t)lock, 0, 0);
        ok &= !nos_syscall(NOS_SYS_MUTEX_DESTROY, (uintptr_t)lock, 0, 0);
    }
#endif
    /* Deliberately leave the map, owned lock and FD for task exit cleanup. */
    exit_task(ok ? 41 : 1);
}
static void resource_regression(void)
{
    struct nos_object_stats before = { 0 }, after = { 0 };
    int fd = nos_open("/tmp/resource-cycle", NOS_O_CREAT | NOS_O_RDWR);
    int ok = fd >= 3 && !nos_close(fd);
    ok &= !nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&before, 0, 0);
    unsigned completed = 0;
    for (; completed < 1000 && ok; completed++) {
        ok &= wait_status(spawn(resource_worker, 0, 8), 41);
        intptr_t lock = nos_syscall(NOS_SYS_MUTEX_CREATE, 0, 0, 0);
        intptr_t mapping = nos_syscall(NOS_SYS_MMAP, NOS_PROT_READ | NOS_PROT_WRITE, NOS_PAGE_SIZE, 0);
        fd = nos_open("/tmp/resource-cycle", NOS_O_RDWR);
        ok &= lock >= 0 && mapping_valid(mapping) && fd >= 3;
        if (lock >= 0) ok &= !nos_syscall(NOS_SYS_MUTEX_DESTROY, (uintptr_t)lock, 0, 0);
        if (mapping_valid(mapping)) ok &= !nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)mapping, 0, 0);
        if (fd >= 3) ok &= !nos_close(fd);
    }
    ok &= !nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&after, 0, 0);
    ok &= before.tasks == after.tasks && before.mutexes == after.mutexes &&
          before.endpoints == after.endpoints && before.free_pages == after.free_pages;
    nos_check(ok && completed == 1000, "resource_lifecycle_1000");
    nos_unlink("/tmp/resource-cycle");
}
#if defined(NOS_RISCV32) && !defined(NOS_MMU)
static void privilege_worker(uintptr_t unused)
{
    (void)unused;
    uintptr_t status;
    __asm__ volatile("csrr %0, mstatus" : "=r"(status));
    exit_task((intptr_t)status);
}
#endif
/* Exercise the real syscall and libc clocks above the signed 32-bit tick
 * boundary in an explicitly selected guest image. */
#ifdef NOS_CLOCK_TEST_EPOCH
static void clock_epoch_regression(void)
{
    uint64_t ticks = 0, later = 0;
    int ok = !nos_syscall(NOS_SYS_TICKS64, (uintptr_t)&ticks, 0, 0) &&
        ticks >= (uint64_t)NOS_CLOCK_TEST_EPOCH;
    nos_check(ok && nos_syscall(NOS_SYS_TICKS64, 1, 0, 0) == -NOS_EFAULT,
              "clock64_kernel_snapshot_boundary");
    intptr_t legacy = nos_syscall(NOS_SYS_TICKS, 0, 0, 0);
#if UINTPTR_MAX == UINT32_MAX
    nos_check(legacy < 0, "clock64_legacy_boundary");
#else
    nos_check((uint64_t)legacy >= (uint64_t)NOS_CLOCK_TEST_EPOCH, "clock64_legacy_boundary");
#endif
#if defined(NOS_NEWLIB) || defined(NOS_MUSL)
    struct timespec before, after, delay = { 0, 20000000 };
    ok = !clock_gettime(CLOCK_MONOTONIC, &before) && before.tv_sec >= (int64_t)(ticks / 100) &&
         !nanosleep(&delay, 0) && !clock_gettime(CLOCK_MONOTONIC, &after) &&
         (after.tv_sec > before.tv_sec || (after.tv_sec == before.tv_sec && after.tv_nsec >= before.tv_nsec + 20000000));
    nos_check(ok, "libc_clock_epoch_boundary");
#else
    nos_syscall(NOS_SYS_SLEEP, 2, 0, 0);
#endif
    nos_check(!nos_syscall(NOS_SYS_TICKS64, (uintptr_t)&later, 0, 0) && later >= ticks + 2,
              "clock64_sleep_progress");
}
#endif
void nos_validation(uintptr_t unused)
{
    (void)unused;
#ifdef NOS_CLOCK_TEST_EPOCH
    clock_epoch_regression();
    nos_syscall(NOS_SYS_FINISH, failures, 0, 0);
    for (;;) { }
#endif
#if NOS_PERSISTENCE_TEST > 0
#if defined(NOS_LINUX_COMPAT) && (NOS_PERSISTENCE_TEST == 8 || NOS_PERSISTENCE_TEST == 9)
    nos_kernel_regression();
#endif
    nos_persistence_regression(NOS_PERSISTENCE_TEST);
    nos_syscall(NOS_SYS_SLEEP, 2, 0, 0);
    nos_syscall(NOS_SYS_FINISH, failures, 0, 0);
    for (;;) { }
#endif
#ifdef NOS_ARM_M
    uintptr_t control;
    __asm__ volatile("mrs %0, control" : "=r"(control));
    nos_check((control & 3) == 3, "nommu_user_privilege");
#endif
#if defined(NOS_RISCV32) && !defined(NOS_MMU)
    nos_check(wait_status(spawn(privilege_worker, 0, 8), -NOS_EFAULT), "nommu_user_privilege");
#endif
    nos_check(nos_syscall(NOS_SYS_GETPID, 0, 0, 0) == 1, "user_syscall");
    nos_check(nos_syscall(999, 0, 0, 0) == -NOS_ENOSYS, "unknown_syscall");
    struct nos_abi_info abi = { 0 };
    nos_check(!nos_syscall(NOS_SYS_ABI_QUERY, (uintptr_t)&abi, 0, 0) &&
              abi.version == NOS_ABI_VERSION && abi.word_bits == sizeof(uintptr_t) * 8 &&
              abi.message_bytes == sizeof(struct nos_message) && abi.page_bytes == NOS_PAGE_SIZE,
              "abi_version_contract");
    nos_check(nos_syscall(NOS_SYS_WRITE, 1, 1, 0) == -NOS_EFAULT &&
              nos_syscall(NOS_SYS_WRITE, UINTPTR_MAX, 2, 0) == -NOS_EFAULT, "user_pointer_validation");
    uintptr_t slot = 0;
    nos_check(nos_syscall(NOS_SYS_TLS_BIND, 1, (uintptr_t)&slot, 0) == -NOS_EFAULT &&
              nos_syscall(NOS_SYS_TLS_BIND, (uintptr_t)&slot, 1, 0) == -NOS_EFAULT, "tls_pointer_validation");
    struct nos_message m = { 0 };
    nos_check(nos_syscall(NOS_SYS_CALL, 99, (uintptr_t)&m, (uintptr_t)&m) == -NOS_EACCES &&
              nos_syscall(NOS_SYS_RECV, 0, (uintptr_t)&m, 0) == -NOS_EACCES &&
              nos_syscall(NOS_SYS_REPLY, 0, (uintptr_t)&m, 0) == -NOS_EPERM, "ipc_capabilities");
    m.op = NOS_IPC_PEER_EXIT;
    nos_check(nos_syscall(NOS_SYS_CALL, 0, (uintptr_t)&m, (uintptr_t)&m) == -NOS_EPERM, "ipc_lifecycle_authentication");
    nos_check(!nos_mkdir("/tmp/test"), "vfs_mkdir");
    int fd = nos_open("/tmp/test/state", NOS_O_CREAT | NOS_O_RDWR);
    const char *content = "microkernel-ipc";
    char buffer[64] = { 0 };
    int ok = fd >= 3 && nos_write(fd, content, 15) == 15 && nos_seek(fd, 0, 0) == 0 &&
             nos_read(fd, buffer, sizeof(buffer) - 1) == 15 && u_streq(buffer, content);
    nos_check(ok, "vfs_rpc_read_write_seek");
    struct nos_stat st;
    nos_check(!nos_stat_fd(fd, &st) && st.size == 15, "vfs_stat");
    nos_check(wait_status(spawn(fd_worker, (uintptr_t)fd, 8), 9), "vfs_fd_ownership");
    nos_check(wait_status(spawn(cleanup_worker, 0, 8), 11) && !nos_unlink("/tmp/exiting-client"), "vfs_process_cleanup");
    nos_check(!nos_close(fd) && nos_close(fd) == -NOS_EBADF && !nos_unlink("/tmp/test/state") &&
              nos_open("/tmp/test/state", 0) == -NOS_ENOENT, "vfs_close_unlink_errors");
    fd = nos_open("/proc/version", NOS_O_RDONLY);
    u_memset(buffer, 0, sizeof(buffer));
    nos_check(fd >= 3 && nos_read(fd, buffer, 63) > 0 && u_streq(buffer, "NOS microkernel\n") &&
              nos_open("/proc/version", NOS_O_RDWR) == -NOS_EACCES, "vfs_mount_procfs");
    nos_close(fd);
    nos_check(nos_open("/tmp/../proc/version", 0) == -NOS_EACCES, "vfs_path_permissions");
    intptr_t p = nos_syscall(NOS_SYS_MMAP, 3, 0, 0);
    ok = mapping_valid(p);
    if (ok) { unsigned char *bytes = (void *)(uintptr_t)p; ok = bytes[0] == 0; bytes[0] = 42; ok &= bytes[0] == 42; }
    nos_check(ok, "anonymous_memory");
#ifdef NOS_MMU
    nos_check(wait_status(spawn(isolated_memory, (uintptr_t)p, 8), 17) &&
              *(unsigned char *)(uintptr_t)p == 42, "private_address_spaces");
#endif
    nos_check(nos_syscall(NOS_SYS_MMAP, 7, 0, 0) == -NOS_EINVAL, "wx_mapping_rejected");
#ifdef NOS_MMU
    nos_check(!nos_syscall(NOS_SYS_MPROTECT, (uintptr_t)p, NOS_PROT_READ, 0), "mmu_page_protection");
#else
    nos_check(nos_syscall(NOS_SYS_MPROTECT, (uintptr_t)p, NOS_PROT_READ, 0) == -NOS_ENOTSUP, "nommu_explicit_contract");
#endif
    nos_check(!nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)p, 0, 0) &&
              nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)p, 0, 0) == -NOS_EINVAL, "memory_unmap");
    p = nos_syscall(NOS_SYS_MMAP, 3, NOS_PAGE_SIZE * 2, 0);
    ok = mapping_valid(p);
    if (ok) {
        unsigned char *bytes = (void *)(uintptr_t)p;
        ok = bytes[NOS_PAGE_SIZE + 3] == 0; bytes[NOS_PAGE_SIZE + 3] = 12;
        ok &= bytes[NOS_PAGE_SIZE + 3] == 12 && !nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)p, 0, 0);
    }
    nos_check(ok, "multi_page_mapping");
    intptr_t blocks[MAPPING_TEST_LIMIT], allocation_error = 0;
    unsigned allocated = 0;
    for (; allocated < MAPPING_TEST_LIMIT; allocated++) {
        blocks[allocated] = nos_syscall(NOS_SYS_MMAP, 3, NOS_PAGE_SIZE, 0);
        if (!mapping_valid(blocks[allocated])) { allocation_error = blocks[allocated]; break; }
    }
    ok = allocated > 0 && allocation_error == -NOS_ENOMEM;
    for (unsigned i = 0; i < allocated; i++) ok &= !nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)blocks[i], 0, 0);
    /* Exhaustion must release its reservation, so a fresh mapping works again. */
    p = nos_syscall(NOS_SYS_MMAP, 3, NOS_PAGE_SIZE, 0);
    ok &= mapping_valid(p) && !nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)p, 0, 0);
    nos_check(ok, "mapping_quota_recovery");
#ifdef NOS_MMU
#ifdef NOS_NEWLIB_DYNAMIC
    struct _reent *saved_reent = (void *)(uintptr_t)nos_syscall(NOS_SYS_TLS_GET, 0, 0, 0);
    extern int nos_newlib_bind_reent(struct _reent *reent);
#endif
    p = nos_syscall(NOS_SYS_MMAP, 3, 0, 0);
    ok = mapping_valid(p) && !nos_syscall(NOS_SYS_TLS_BIND, (uintptr_t)p, (uintptr_t)&slot, 0);
    ok &= !nos_syscall(NOS_SYS_MPROTECT, (uintptr_t)p, 1, 0);
    ok &= !nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)p, 0, 0);
    ok &= nos_syscall(NOS_SYS_GETPID, 0, 0, 0) == 1;
#ifdef NOS_NEWLIB_DYNAMIC
    ok &= !nos_newlib_bind_reent(saved_reent);
#endif
    nos_check(ok, "tls_mapping_revocation");
#endif
    intptr_t brk = nos_syscall(NOS_SYS_BRK, 0, 0, 0);
    nos_check(mapping_valid(brk) && nos_syscall(NOS_SYS_BRK, UINTPTR_MAX, 0, 0) == -NOS_ENOMEM, "heap_bounds");
    intptr_t worker = spawn(sleep_worker, 0, 8);
    intptr_t tick = nos_syscall(NOS_SYS_TICKS, 0, 0, 0);
    while (nos_syscall(NOS_SYS_TICKS, 0, 0, 0) - tick < 6) {
        for (volatile unsigned n = 0; n < 1000; n++) { }
    }
    nos_check(wait_status(worker, 23), "scheduler_sleep_wait");
    intptr_t invalid_status;
    nos_check(nos_syscall(NOS_SYS_WAIT, (uintptr_t)worker, (uintptr_t)&invalid_status, 0) == -NOS_ESRCH, "process_stale_handle");
#if defined(NOS_ARM) || defined(NOS_ARM64) || defined(NOS_RISCV)
    nos_fp_set(0x3ff4000000000000ull);
    worker = spawn(fp_worker, 0, 8);
    nos_syscall(NOS_SYS_SLEEP, 4, 0, 0);
    nos_check(nos_fp_get() == 0x3ff4000000000000ull && wait_status(worker, 19), "fpu_context_switch");
#endif
    intptr_t lock = nos_syscall(NOS_SYS_MUTEX_CREATE, 0, 0, 0);
    ok = lock >= 0 && !nos_syscall(NOS_SYS_MUTEX_LOCK, (uintptr_t)lock, 0, 0) &&
         nos_syscall(NOS_SYS_MUTEX_LOCK, (uintptr_t)lock, 0, 0) == -NOS_EDEADLK &&
         nos_syscall(NOS_SYS_MUTEX_DESTROY, (uintptr_t)lock, 0, 0) == -NOS_EBUSY;
    worker = spawn(lock_worker, (uintptr_t)lock, 6);
    intptr_t donation_deadline = nos_syscall(NOS_SYS_TICKS, 0, 0, 0) + 30;
    while (nos_syscall(NOS_SYS_PRIORITY, 0, 0, 0) != 6 &&
           nos_syscall(NOS_SYS_TICKS, 0, 0, 0) < donation_deadline)
        nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    ok &= nos_syscall(NOS_SYS_PRIORITY, 0, 0, 0) == 6 && nos_syscall(NOS_SYS_PRIORITY, 1, 0, 0) == 8;
    ok &= !nos_syscall(NOS_SYS_MUTEX_UNLOCK, (uintptr_t)lock, 0, 0) && wait_status(worker, 7);
    ok &= nos_syscall(NOS_SYS_PRIORITY, 0, 0, 0) == 8;
    ok &= !nos_syscall(NOS_SYS_MUTEX_DESTROY, (uintptr_t)lock, 0, 0);
    nos_check(ok, "mutex_blocking_priority_inheritance");
    nos_check(nos_syscall(NOS_SYS_MUTEX_LOCK, (uintptr_t)lock, 0, 0) == -NOS_EINVAL, "mutex_stale_handle");
#if UINTPTR_MAX > UINT32_MAX
    lock = nos_syscall(NOS_SYS_MUTEX_CREATE, 0, 0, 0);
    uintptr_t high_word = (uintptr_t)1 << 32;
    ok = lock >= 0 && nos_syscall(NOS_SYS_MUTEX_LOCK, (uintptr_t)lock + high_word, 0, 0) == -NOS_EINVAL;
    ok &= nos_syscall(NOS_SYS_MMAP, NOS_PROT_READ | high_word, NOS_PAGE_SIZE, 0) == -NOS_EINVAL;
    ok &= nos_syscall(NOS_SYS_WAIT, 1 + high_word, (uintptr_t)&invalid_status, 0) == -NOS_ESRCH;
    if (lock >= 0) ok &= !nos_syscall(NOS_SYS_MUTEX_DESTROY, (uintptr_t)lock, 0, 0);
    nos_check(ok, "native_width_handle_validation");
#endif
#ifdef NOS_MMU
#if defined(NOS_RISCV)
    uintptr_t kernel = 0x80000000;
#else
    uintptr_t kernel = 0x40010000;
#endif
    nos_check(wait_status(spawn(kernel_fault, kernel, 8), -NOS_EFAULT), "mmu_kernel_isolation");
    nos_check(wait_status(spawn(write_fault, 0, 8), -NOS_EFAULT), "mmu_write_fault_recovery");
    nos_check(wait_status(spawn(execute_fault, 0, 8), -NOS_EFAULT), "mmu_execute_never");
    nos_check(nos_open("/proc/version", 0) >= 3, "service_survives_client_fault");
#endif
#if defined(NOS_NEWLIB) || defined(NOS_MUSL)
    char *heap = malloc(256);
    if (heap) { heap[0] = 73; heap[255] = 91; }
    nos_check(heap && heap[0] == 73 && heap[255] == 91, LIBC_TAG "_malloc");
    free(heap);
    int printed = printf(LIBC_TAG " user printf %d\n", 42);
    nos_check(printed > 0 && !fflush(stdout), LIBC_TAG "_printf");
    FILE *file = fopen("/tmp/libc", "w+");
    char buf[8] = { 0 };
    ok = file && fwrite("stdio", 1, 5, file) == 5 && !fflush(file) && !fseek(file, 0, SEEK_SET) &&
         fread(buf, 1, 5, file) == 5 && u_streq(buf, "stdio");
    if (file) fclose(file);
    nos_check(ok, LIBC_TAG "_stdio_vfs");
    char formatted[32];
    nos_check(snprintf(formatted, sizeof(formatted), "%.2f", 1.25) == 4 && u_streq(formatted, "1.25"), LIBC_TAG "_float_format");
    errno = 61;
    worker = spawn(libc_worker, 0, 8);
    nos_syscall(NOS_SYS_SLEEP, 4, 0, 0);
#ifdef NOS_MMU
    nos_check(errno == 61 && wait_status(worker, 29), LIBC_TAG "_process_tls");
#else
    nos_check(errno == 61 && wait_status(worker, 29), LIBC_TAG "_thread_reent");
#endif
#ifdef NOS_MUSL
    heap = malloc(32768);
    if (heap) { heap[0] = 18; heap[32767] = 39; }
    nos_check(heap && heap[0] == 18 && heap[32767] == 39, "musl_large_allocation");
    free(heap);
#endif
#endif
    nos_vfs_regression();
    nos_libc_regression();
    nos_arch_regression();
    nos_elf_regression();
    resource_regression();
    nos_kernel_regression();
#ifdef NOS_POSIX_SUITE
    nos_posix_suite();
#elif defined(NOS_LINUX_COMPAT)
    nos_linux_regression();
#endif
    nos_syscall(NOS_SYS_FINISH, failures, 0, 0);
    for (;;) { }
}
