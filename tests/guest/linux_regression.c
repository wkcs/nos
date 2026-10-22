/* Copyright (C) 2026 NOS contributors */
#include "init.h"
#include "linux.h"
#include "thread.h"
#include "user.h"
struct capacity_monitor {
    unsigned stop, samples, minimum_free, peak_tasks;
};
static void *capacity_sample(void *argument)
{
    struct capacity_monitor *monitor = argument;
    /* Bounded independently of the child's alarm. The ordinary native join
     * waits for clear_tid, then unmaps this monitor's private 8 KiB stack. */
    for (unsigned attempt = 0; attempt < 12000; attempt++) {
        if (__atomic_load_n(&monitor->stop, __ATOMIC_ACQUIRE)) return 0;
        struct nos_object_stats stats;
        if (nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&stats, 0, 0)) return (void *)1;
        if (stats.free_pages < monitor->minimum_free) monitor->minimum_free = stats.free_pages;
        if (stats.tasks > monitor->peak_tasks) monitor->peak_tasks = stats.tasks;
        monitor->samples++;
        nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    }
    return (void *)2;
}
static int capacity_stats_equal(const struct nos_object_stats *one, const struct nos_object_stats *two)
{
    return one->tasks == two->tasks && one->mutexes == two->mutexes &&
        one->endpoints == two->endpoints && one->free_pages == two->free_pages;
}
static int capacity_collect(const struct nos_object_stats *expected, struct nos_object_stats *result)
{
    unsigned stable = 0;
    struct nos_object_stats previous = {0};
    for (unsigned attempt = 0; attempt < 128; attempt++) {
        /* VFS collects dead process owners during service. A real stat RPC
         * drives that cleanup; sleeping alone is not a collection barrier. */
        struct nos_stat64 root;
        if (nos_stat_at(NOS_AT_FDCWD, "/", 0, &root) ||
            nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)result, 0, 0)) return 0;
        if (expected && capacity_stats_equal(expected, result)) return 1;
        stable = capacity_stats_equal(&previous, result) ? stable + 1 : 0;
        if (!expected && stable == 4) return 1;
        previous = *result;
    }
    return 0;
}
static void capacity_number(const char *label, unsigned value)
{
    char digits[16]; unsigned used = sizeof(digits);
    digits[--used] = 0;
    do { digits[--used] = (char)('0' + value % 10); value /= 10; } while (value);
    nos_puts(label); nos_puts(digits + used);
}
static unsigned capacity_path(const char *path)
{
    if (u_streq(path, "/bin/glibc_capacity")) return 1;
    if (u_streq(path, "/bin/glibc_process_capacity")) return 2;
    return 0;
}
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
static int linux_entropy(unsigned char *random)
{
    for (size_t copied = 0; copied < 16;) {
        struct nos_message entropy = {.op = NOS_VFS_GETRANDOM, .len = 16 - copied};
        intptr_t rc = nos_rpc(&entropy);
        if (rc < 0) return (int)rc;
        if (!rc || (uintptr_t)rc > 16 - copied || entropy.len != (size_t)rc) return -NOS_EIO;
        u_memcpy(random + copied, entropy.data, (size_t)rc);
        copied += (size_t)rc;
    }
    return 0;
}
static int execute_linux(const char *path, const unsigned char *supplied_random)
{
    unsigned char random[16];
    if (supplied_random) u_memcpy(random, supplied_random, sizeof(random));
    else {
        int rc = linux_entropy(random);
        if (rc) return rc;
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
    intptr_t rc = nos_syscall(NOS_SYS_WAIT, (uintptr_t)pid, (uintptr_t)&status, 0);
    return rc < 0 ? (int)rc : (int)status;
}
static int run_linux(const char *path)
{
    unsigned kind = capacity_path(path);
    if (!kind) return execute_linux(path, 0);
    /* Initialize the persistent virtio-rng DMA storage before measuring
     * application resources. These fresh bytes are consumed by this exec. */
    unsigned char random[16];
    int rc = linux_entropy(random);
    if (rc) return rc;
    struct nos_object_stats before, after = {0};
    if (!capacity_collect(0, &before)) return -NOS_EIO;
    struct capacity_monitor monitor = {.minimum_free = before.free_pages, .peak_tasks = before.tasks};
    struct nos_thread sampler;
    rc = nos_thread_create(&sampler, capacity_sample, &monitor);
    if (rc) return rc;
    int status = execute_linux(path, random);
    __atomic_store_n(&monitor.stop, 1, __ATOMIC_RELEASE);
    void *sample_result = (void *)3;
    int joined = !nos_thread_join(&sampler, &sample_result);
    int collected = capacity_collect(&before, &after) && joined;
    capacity_number(kind == 2 ? "glibc_process_capacity_resources free_before=" :
                    "glibc_capacity_resources free_before=", before.free_pages);
    capacity_number(" free_min=", monitor.minimum_free);
    capacity_number(" free_after=", after.free_pages);
    capacity_number(" tasks_before=", before.tasks);
    capacity_number(" tasks_peak=", monitor.peak_tasks);
    capacity_number(" tasks_after=", after.tasks);
    nos_puts(" monitor_stack_bytes=8192\n");
    int sampled = joined && !sample_result && monitor.samples &&
        monitor.minimum_free < before.free_pages && monitor.peak_tasks >= before.tasks + (kind == 2 ? 27u : 66u);
    nos_check(sampled, kind == 2 ? "glibc_process_capacity_native_peak_sample" : "glibc_capacity_native_peak_sample");
    nos_check(collected, kind == 2 ? "glibc_process_capacity_process_exit_page_baseline" : "glibc_capacity_process_exit_page_baseline");
    return status ? status : sampled && collected ? 0 : -NOS_EIO;
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
    result = run_linux("/bin/glibc_path4096");
    failed("/bin/glibc_path4096", result);
    nos_check(result == 0, "glibc_path4096_application");
    result = run_linux("/bin/glibc_resources");
    failed("/bin/glibc_resources", result);
    nos_check(result == 0, "glibc_resources_application");
    result = run_linux("/bin/glibc_address_space");
    failed("/bin/glibc_address_space", result);
    nos_check(result == 0, "glibc_address_space_application");
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
    result = run_linux("/bin/glibc_process_capacity");
    failed("/bin/glibc_process_capacity", result);
    nos_check(result == 0, "glibc_process_capacity_application");
    result = run_linux("/bin/glibc_nproc");
    failed("/bin/glibc_nproc", result);
    nos_check(result == 0, "glibc_nproc_application");
    result = run_linux("/bin/glibc_capacity");
    failed("/bin/glibc_capacity", result);
    nos_check(result == 0, "glibc_capacity_application");
#endif
}
