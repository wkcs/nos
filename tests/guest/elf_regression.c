/* Copyright (C) 2026 NOS contributors */
#include "init.h"
static int execute_argument(const char *path, const char *argument)
{
    const char *arguments[] = {path, argument, 0};
    const char *environment[] = {"NOS=regression", "PATH=/bin", 0};
    struct nos_exec_request request = {path, arguments, environment, 8};
    return (int)nos_syscall(NOS_SYS_EXEC_SPAWN, (uintptr_t)&request, 0, 0);
}
static int execute(const char *path) { return execute_argument(path, "qemu"); }
static int wait_status(int pid)
{
    intptr_t status = -1;
    intptr_t rc = nos_syscall(NOS_SYS_WAIT, (uintptr_t)pid, (uintptr_t)&status, 0);
    return rc < 0 ? (int)rc : (int)status;
}
void nos_elf_regression(void)
{
    struct nos_object_stats before, after;
    nos_check(!nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&before, 0, 0), "elf_object_baseline");
    int pid = execute("/bin/hello");
    nos_check(pid >= 0 && wait_status(pid) == 23, "elf_independent_app");
    static const char *invalid[] = {"/test/elf-bounds", "/test/elf-wx", "/test/elf-entry",
        "/test/elf-overlap", "/test/elf-machine", "/test/elf-tls"};
    unsigned rejected = 0;
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        pid = execute(invalid[i]);
        if (pid < 0) rejected++;
        else wait_status(pid);
    }
    nos_check(rejected == sizeof(invalid) / sizeof(invalid[0]), "elf_malformed_rejected");
    pid = execute("/test/elf-entry-bss");
    nos_check(pid < 0, "elf_entry_file_backed");
    if (pid >= 0) wait_status(pid);
    static const char *boundaries[] = {"/test/elf-segment-wrap", "/test/elf-metadata-bounds",
        "/test/elf-tls-escape", "/test/elf-exec-stack"};
    unsigned safe = 0;
    for (unsigned i = 0; i < sizeof(boundaries) / sizeof(boundaries[0]); i++) {
        pid = execute(boundaries[i]);
        if (pid < 0) safe++; else wait_status(pid);
    }
    nos_check(safe == sizeof(boundaries) / sizeof(boundaries[0]), "elf_segment_boundaries");
#ifndef NOS_MMU
    static const char *relocations[] = {"/test/elf-rel-text", "/test/elf-dynamic-unterminated",
        "/test/elf-dynamic-bss", "/test/elf-rel-table-bss"};
    safe = 0;
    for (unsigned i = 0; i < sizeof(relocations) / sizeof(relocations[0]); i++) {
        pid = execute(relocations[i]);
        if (pid < 0) safe++; else wait_status(pid);
    }
    nos_check(safe == sizeof(relocations) / sizeof(relocations[0]), "elf_pie_relocations_validated");
#else
    pid = execute_argument("/bin/hello", "nx");
    nos_check(pid >= 0 && wait_status(pid) == -NOS_EFAULT, "elf_rodata_execute_never");
#endif
    struct nos_exec_request bad = {(const char *)(uintptr_t)1, 0, 0, 8};
    nos_check(nos_syscall(NOS_SYS_EXEC_SPAWN, (uintptr_t)&bad, 0, 0) == -NOS_EFAULT,
              "elf_arguments_validated");
    nos_check(execute("/bin/missing") == -NOS_ENOENT, "elf_missing_image");
    nos_check(!nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&after, 0, 0) &&
              before.free_pages == after.free_pages && before.tasks == after.tasks,
              "elf_failed_load_reclaims_pages");
#if (defined(NOS_NEWLIB) || defined(NOS_MUSL)) && !defined(NOS_ARM_M)
    pid = execute("/bin/libc_check");
    nos_check(pid >= 0 && wait_status(pid) == 0, "elf_libc_tls_app");
#endif
#if NOS_EXTRA_APPS
    pid = execute("/bin/lua");
    nos_check(pid >= 0 && wait_status(pid) == 0, "elf_lua_app");
    pid = execute("/bin/lvgl");
    nos_check(pid >= 0 && wait_status(pid) == 0, "elf_lvgl_app");
#endif
}
