/* Copyright (C) 2026 NOS contributors */
#include "init.h"
#include "linux.h"
#include "posix_cases.h"
#ifndef NOS_POSIX_CASE_TICKS
#define NOS_POSIX_CASE_TICKS 3000u
#endif
_Static_assert(NOS_POSIX_CASE_COUNT > 0, "an empty POSIX selection is not a test run");
static void print_number(intptr_t value)
{
    char buffer[3 * sizeof(value) + 3];
    size_t n = 0;
    uintptr_t magnitude = (uintptr_t)value;
    if (value < 0) { nos_puts("-"); magnitude = 0 - magnitude; }
    do { buffer[n++] = (char)('0' + magnitude % 10); magnitude /= 10; } while (magnitude);
    while (n) { char text[2] = {buffer[--n], 0}; nos_puts(text); }
}
static void report_case(const char *name, intptr_t status)
{
    nos_puts("NOS POSIX RESULT "); nos_puts(name); nos_puts(" "); print_number(status); nos_puts("\n");
}
static int snapshot(uint64_t *ticks)
{ return (int)nos_syscall(NOS_SYS_TICKS64, (uintptr_t)ticks, 0, 0); }
static intptr_t start_case(const char *path)
{
    unsigned char random[16];
    size_t received = 0;
    while (received < sizeof(random)) {
        struct nos_message entropy = { .op = NOS_VFS_GETRANDOM, .len = sizeof(random) - received };
        size_t wanted = entropy.len;
        intptr_t result = nos_rpc(&entropy);
        if (result < 0) return result;
        if (!result || (size_t)result > wanted || entropy.len < (size_t)result) return -NOS_EIO;
        u_memcpy(random + received, entropy.data, (size_t)result);
        received += (size_t)result;
    }
    const char *arguments[] = {path, 0};
    const char *environment[] = {"LD_LIBRARY_PATH=/lib", "LC_ALL=C", 0};
    struct nos_exec_request request = {path, arguments, environment, 8};
    return nos_syscall(NOS_SYS_EXEC_LINUX, (uintptr_t)&request, (uintptr_t)random, NOS_EXEC_RANDOM_VALID);
}
void nos_posix_suite(void)
{
    unsigned passed = 0;
    for (size_t i = 0; i < NOS_POSIX_CASE_COUNT; i++) {
        const char *name = nos_posix_cases[i].name;
        intptr_t pid = start_case(nos_posix_cases[i].path), status = -NOS_EIO;
        if (pid < 0) { report_case(name, pid); continue; }
        uint64_t start, now;
        int clock_error = snapshot(&start);
        unsigned timed_out = 0;
        for (;;) {
            intptr_t result = nos_syscall(NOS_SYS_WAIT, (uintptr_t)pid, (uintptr_t)&status, NOS_WAIT_NONBLOCK);
            if (!result) break;
            if (result != -NOS_EAGAIN) { status = result; break; }
            if (clock_error || snapshot(&now) || now - start >= NOS_POSIX_CASE_TICKS) {
                timed_out = 1;
                nos_puts("NOS POSIX TIMEOUT "); nos_puts(name); nos_puts("\n");
                intptr_t cancelled = nos_syscall(NOS_SYS_TASK_CANCEL, (uintptr_t)pid, NOS_CANCEL_GROUP, 0);
                if (cancelled) {
                    nos_puts("NOS POSIX CLEANUP_ERROR "); nos_puts(name); nos_puts(" ");
                    print_number(cancelled); nos_puts("\n"); break;
                }
                /* Wait for actual group quiescence, including remote CPUs,
                 * before reusing its task/address-space/descriptor slots. */
                intptr_t waited = nos_syscall(NOS_SYS_WAIT, (uintptr_t)pid, (uintptr_t)&status, 0);
                if (waited) {
                    nos_puts("NOS POSIX CLEANUP_ERROR "); nos_puts(name); nos_puts(" ");
                    print_number(waited); nos_puts("\n");
                }
                break;
            }
            intptr_t slept = nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
            if (slept && slept != -NOS_EINTR) { clock_error = 1; }
        }
        if (!timed_out) { report_case(name, status); passed += status == 0; }
    }
    /* Upstream PTS_FAIL/UNRESOLVED/UNSUPPORTED/UNTESTED and guest timeouts
     * remain distinct in the host report. None counts toward this PASS. */
    nos_check(passed == NOS_POSIX_CASE_COUNT, "open_posix_selected_cases");
}
