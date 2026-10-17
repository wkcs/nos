/* Copyright (C) 2026 NOS contributors */
#include "init.h"
#include "user.h"
#include "linux.h"
static unsigned service_pid;
#define NOS_SHELL_ARGC 8u

int nos_exec(const char *path, const char *const *argv, const char *const *envp, unsigned priority)
{
    struct nos_exec_request request = {path, argv, envp, priority};
    return (int)nos_syscall(NOS_SYS_EXEC_SPAWN, (uintptr_t)&request, 0, 0);
}
static void print_number(intptr_t value)
{
    char digits[24]; unsigned i = sizeof(digits); uintptr_t magnitude;
    digits[--i] = 0;
    if (value < 0) { nos_puts("-"); magnitude = (uintptr_t)(-(value + 1)) + 1; }
    else magnitude = (uintptr_t)value;
    do { digits[--i] = (char)('0' + magnitude % 10); magnitude /= 10; } while (magnitude);
    nos_puts(digits + i);
}
static void report(const char *operation, intptr_t result)
{
    nos_puts(operation); nos_puts(": "); print_number(result); nos_puts("\n");
}
static void uart_stats(void)
{
    struct { uint64_t interrupts; uint32_t received, dropped, ready, reserved; } stats;
    struct nos_message message = {.op = NOS_VFS_DEVICE_STATS, .arg = 1};
    if (nos_rpc(&message) || message.len != sizeof(stats)) return;
    u_memcpy(&stats, message.data, sizeof(stats));
    char digits[21]; unsigned i = sizeof(digits); uint64_t number = stats.interrupts;
    digits[--i] = 0;
    do { digits[--i] = (char)('0' + number % 10); number /= 10; } while (number);
    nos_puts("uart_irq: "); nos_puts(digits + i); nos_puts("\n");
    report("uart_rx", stats.received); report("uart_dropped", stats.dropped); report("uart_ready", stats.ready);
}
static void restart_service(void)
{
    intptr_t rc = nos_syscall(NOS_SYS_TASK_CANCEL, service_pid, 0, 0);
    if (rc && rc != -NOS_ESRCH) { report("restart", rc); return; }
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        rc = nos_syscall(NOS_SYS_SERVICE_RESTART, 0, 0, 0);
        if (rc != -NOS_EBUSY) break;
        nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    }
    if (rc >= 0) service_pid = (unsigned)rc;
    report("restart", rc);
}
static int exec_linux(const char *path, const char *const *argv, const char *const *envp)
{
#ifdef NOS_LINUX_COMPAT
    unsigned char random[16];
    for (size_t copied = 0; copied < sizeof(random);) {
        struct nos_message entropy = {.op = NOS_VFS_GETRANDOM, .len = sizeof(random) - copied};
        intptr_t rc = nos_rpc(&entropy);
        if (rc < 0) return (int)rc;
        if (!rc || (uintptr_t)rc > sizeof(random) - copied || entropy.len != (size_t)rc) return -NOS_EIO;
        u_memcpy(random + copied, entropy.data, (size_t)rc);
        copied += (size_t)rc;
    }
    struct nos_exec_request request = {path, argv, envp, 8};
    return (int)nos_syscall(NOS_SYS_EXEC_LINUX, (uintptr_t)&request,
                            (uintptr_t)random, NOS_EXEC_RANDOM_VALID);
#else
    (void)path; (void)argv; (void)envp; return -NOS_ENOTSUP;
#endif
}
static void run_program(unsigned count, char **words, unsigned linux_abi)
{
    const char *arguments[NOS_SHELL_ARGC + 1];
    if (!count || count > NOS_SHELL_ARGC) { nos_puts("run /bin/program [args]\n"); return; }
    for (unsigned i = 0; i < count; i++) arguments[i] = words[i];
    arguments[count] = 0;
    const char *environment[] = {"PATH=/bin", "NOS=1", "LD_LIBRARY_PATH=/lib", 0};
    int pid = linux_abi ? exec_linux(words[0], arguments, environment) : nos_exec(words[0], arguments, environment, 8);
    if (pid < 0) { report("exec", pid); return; }
    intptr_t status = 0;
    intptr_t rc = nos_syscall(NOS_SYS_WAIT, (uintptr_t)pid, (uintptr_t)&status, 0);
    report("program", rc < 0 ? rc : status);
}
static void shell_command(char *line)
{
    char *words[NOS_SHELL_ARGC + 1]; unsigned count = 0;
    char *p = line;
    while (*p && count < NOS_SHELL_ARGC + 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        words[count++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) *p++ = 0;
    }
    if (!count) return;
    if (u_streq(words[0], "help")) {
        nos_puts("help | ls [path] | cat path | write path text | mkdir path | mv old new\n"
                 "rm path | rmdir path | mount [path] | sync | run /bin/program [args] | stats | restart\n");
#ifdef NOS_LINUX_COMPAT
        nos_puts("runlinux /bin/program [args]\n");
#endif
    } else if (u_streq(words[0], "ls")) {
        int fd = nos_open(count > 1 ? words[1] : "/", NOS_O_RDONLY);
        if (fd < 0) { report("ls", fd); return; }
        char name[NOS_MSG_DATA]; int rc;
        for (size_t i = 0; (rc = nos_readdir(fd, i, name, sizeof(name))) > 0; i++) {
            nos_puts(name); nos_puts("\n");
        }
        if (rc < 0) report("ls", rc);
        nos_close(fd);
    } else if (u_streq(words[0], "cat") && count == 2) {
        int fd = nos_open(words[1], NOS_O_RDONLY);
        if (fd < 0) { report("cat", fd); return; }
        char buffer[NOS_MSG_DATA]; intptr_t rc;
        while ((rc = nos_read(fd, buffer, sizeof(buffer))) > 0) nos_write(1, buffer, (size_t)rc);
        if (rc < 0) report("cat", rc);
        else nos_puts("\n");
        nos_close(fd);
    } else if (u_streq(words[0], "write") && count >= 3) {
        int fd = nos_open(words[1], NOS_O_WRONLY | NOS_O_CREAT | NOS_O_TRUNC);
        if (fd < 0) { report("write", fd); return; }
        intptr_t result = 0;
        for (unsigned i = 2; i < count; i++) {
            if (i > 2 && nos_write(fd, " ", 1) != 1) { result = -NOS_EIO; break; }
            intptr_t rc = nos_write(fd, words[i], u_strlen(words[i]));
            if (rc < 0) { result = rc; break; }
            result += rc;
        }
        nos_close(fd); report("write", result);
    } else if (u_streq(words[0], "mkdir") && count == 2) report("mkdir", nos_mkdir(words[1]));
    else if (u_streq(words[0], "mv") && count == 3) report("mv", nos_rename(words[1], words[2]));
    else if (u_streq(words[0], "rm") && count == 2) report("rm", nos_unlink(words[1]));
    else if (u_streq(words[0], "rmdir") && count == 2) report("rmdir", nos_rmdir(words[1]));
    else if (u_streq(words[0], "mount") && count <= 2) report("mount", nos_mount(count == 2 ? words[1] : "/persist", 1));
    else if (u_streq(words[0], "sync") && count == 1) report("sync", nos_sync());
    else if (u_streq(words[0], "run")) run_program(count - 1, words + 1, 0);
#ifdef NOS_LINUX_COMPAT
    else if (u_streq(words[0], "runlinux")) run_program(count - 1, words + 1, 1);
#endif
    else if (u_streq(words[0], "restart") && count == 1) restart_service();
    else if (u_streq(words[0], "stats")) {
        struct nos_object_stats stats;
        intptr_t rc = nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&stats, 0, 0);
        if (rc < 0) report("stats", rc);
        else { report("tasks", stats.tasks); report("mutexes", stats.mutexes); report("endpoints", stats.endpoints); report("free_pages", stats.free_pages); }
        uart_stats();
    } else nos_puts("invalid command; type help\n");
}
void nos_init(uintptr_t argument)
{
    (void)argument;
    nos_puts("NOS NORMAL BOOT: supervised VFS and independent ELF applications\n");
    char *boot_arguments[] = {"/bin/hello", "boot"};
    run_program(2, boot_arguments, 0);
    nos_puts("NOS SHELL READY\nnos> ");
    char line[256]; unsigned length = 0, dropping = 0;
    for (;;) {
        char character;
        intptr_t rc = nos_read(0, &character, 1);
        if (rc == -NOS_EAGAIN || !rc) { nos_syscall(NOS_SYS_SLEEP, 1, 0, 0); continue; }
        if (rc < 0) {
            intptr_t restarted = nos_syscall(NOS_SYS_SERVICE_RESTART, 0, 0, 0);
            if (restarted >= 0) { service_pid = (unsigned)restarted; nos_puts("VFS restarted\n"); }
            nos_syscall(NOS_SYS_SLEEP, 10, 0, 0); continue;
        }
        if (character == '\r' || character == '\n') {
            nos_puts("\n"); line[length] = 0;
            if (!dropping) shell_command(line);
            else nos_puts("command too long\n");
            length = dropping = 0; nos_puts("nos> ");
        } else if (character == 8 || character == 127) {
            if (length) { length--; nos_puts("\b \b"); }
        } else if ((unsigned char)character >= 32 && (unsigned char)character < 127) {
            if (length + 1 < sizeof(line)) { line[length++] = character; nos_write(1, &character, 1); }
            else dropping = 1;
        }
    }
}
