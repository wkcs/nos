/* Copyright (C) 2026 NOS contributors */
#include "init.h"
#include "linux.h"
static void failed(const char *path, int status)
{
    if (!status) return;
    char digits[16]; unsigned n = sizeof(digits), value = status < 0 ? (unsigned)(-(status + 1)) + 1 : (unsigned)status;
    digits[--n] = 0;
    do { digits[--n] = (char)('0' + value % 10); value /= 10; } while (value);
    nos_puts("Linux application "); nos_puts(path); nos_puts(" status ");
    if (status < 0) nos_puts("-");
    nos_puts(digits + n); nos_puts("\n");
}
static int run_linux(const char *path)
{
    unsigned char random[16];
    intptr_t rc;
    for (size_t copied = 0; copied < sizeof(random);) {
        struct nos_message entropy = {.op = NOS_VFS_GETRANDOM, .len = sizeof(random) - copied};
        rc = nos_rpc(&entropy);
        if (rc < 0) return (int)rc;
        if (!rc || (uintptr_t)rc > sizeof(random) - copied || entropy.len != (size_t)rc) return -NOS_EIO;
        u_memcpy(random + copied, entropy.data, (size_t)rc);
        copied += (size_t)rc;
    }
    const char *arguments[] = {path, "qemu", 0};
    char expected_cpus[] = "NOS_EXPECT_CPUS=1";
    intptr_t cpus = nos_syscall(NOS_SYS_CPU_QUERY, 1, 0, 0);
    if (cpus >= 1 && cpus <= 4) expected_cpus[sizeof(expected_cpus) - 2] = (char)('0' + cpus);
    const char *environment[] = {"LD_LIBRARY_PATH=/lib", "NOS=linux", expected_cpus, 0};
    struct nos_exec_request request = {path, arguments, environment, 8};
    intptr_t pid = nos_syscall(NOS_SYS_EXEC_LINUX, (uintptr_t)&request, (uintptr_t)random, NOS_EXEC_RANDOM_VALID);
    if (pid < 0) { failed("exec loader", (int)pid); return (int)pid; }
    intptr_t status = -1;
    rc = nos_syscall(NOS_SYS_WAIT, (uintptr_t)pid, (uintptr_t)&status, 0);
    return rc < 0 ? (int)rc : (int)status;
}
void nos_linux_regression(void)
{
#ifdef NOS_LINUX_FOCUSED_APP
    int focused = run_linux(NOS_LINUX_FOCUSED_APP);
    failed(NOS_LINUX_FOCUSED_APP, focused);
    nos_check(focused == 0, "linux_focused_application");
#else
    const char *arguments[] = {"/bin/dynamic_check", "qemu", 0};
    struct nos_exec_request request = {arguments[0], arguments, 0, 8};
    nos_check(nos_syscall(NOS_SYS_EXEC_LINUX, (uintptr_t)&request, 0, 0) == -NOS_EAGAIN, "linux_exec_requires_entropy");
    int result = run_linux("/bin/dynamic_check");
    failed("/bin/dynamic_check", result);
    nos_check(result == 0, "linux_dynamic_interpreter");
    result = run_linux("/bin/glibc_check");
    failed("/bin/glibc_check", result);
    nos_check(result == 0, "glibc_dynamic_application");
    result = run_linux("/bin/glibc_poll");
    failed("/bin/glibc_poll", result);
    nos_check(result == 0, "glibc_poll_application");
    result = run_linux("/bin/glibc_credentials");
    failed("/bin/glibc_credentials", result);
    nos_check(result == 0, "glibc_credentials_application");
    result = run_linux("/bin/glibc_permissions");
    failed("/bin/glibc_permissions", result);
    nos_check(result == 0, "glibc_permissions_application");
    result = run_linux("/bin/glibc_pipe_atomic");
    failed("/bin/glibc_pipe_atomic", result);
    nos_check(result == 0, "glibc_pipe_atomic_application");
    result = run_linux("/bin/glibc_signals");
    failed("/bin/glibc_signals", result);
    nos_check(result == 0, "glibc_signal_application");
    result = run_linux("/bin/glibc_process");
    failed("/bin/glibc_process", result);
    nos_check(result == 0, "glibc_process_application");
    result = run_linux("/bin/glibc_futex_shared");
    failed("/bin/glibc_futex_shared", result);
    nos_check(result == 0, "glibc_futex_shared_application");
    result = run_linux("/bin/glibc_shared_sync");
    failed("/bin/glibc_shared_sync", result);
    nos_check(result == 0, "glibc_shared_sync_application");
    result = run_linux("/bin/glibc_files");
    failed("/bin/glibc_files", result);
    nos_check(result == 0, "glibc_files_application");
    result = run_linux("/bin/glibc_paths");
    failed("/bin/glibc_paths", result);
    nos_check(result == 0, "glibc_paths_application");
    result = run_linux("/bin/glibc_record_locks");
    failed("/bin/glibc_record_locks", result);
    nos_check(result == 0, "glibc_record_locks_application");
    result = run_linux("/bin/glibc_record_lock_lifecycle");
    failed("/bin/glibc_record_lock_lifecycle", result);
    nos_check(result == 0, "glibc_record_lock_lifecycle_application");
    result = run_linux("/bin/glibc_sync");
    failed("/bin/glibc_sync", result);
    nos_check(result == 0, "glibc_sync_application");
    result = run_linux("/bin/glibc_threads");
    failed("/bin/glibc_threads", result);
    nos_check(result == 0, "glibc_nptl_threads");
#endif
}
