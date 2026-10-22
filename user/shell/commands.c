/* Copyright (C) 2026 NOS contributors */
#include "commands.h"
#include "init.h"
#include "linux.h"

intptr_t shell_write_all(int fd, const char *buffer, size_t length)
{
    size_t written = 0;
    while (written < length) {
        intptr_t result = nos_write(fd, buffer + written, length - written);
        if (result < 0) return result;
        if (!result || (uintptr_t)result > length - written) return -NOS_EIO;
        written += (size_t)result;
    }
    return (intptr_t)written;
}

static void print_number(intptr_t value)
{
    char digits[24];
    unsigned index = sizeof(digits);
    uintptr_t magnitude;
    digits[--index] = 0;
    if (value < 0) {
        nos_puts("-");
        magnitude = (uintptr_t)(-(value + 1)) + 1;
    } else magnitude = (uintptr_t)value;
    do {
        digits[--index] = (char)('0' + magnitude % 10);
        magnitude /= 10;
    } while (magnitude);
    nos_puts(digits + index);
}

void shell_report(const char *operation, intptr_t result)
{
    nos_puts(operation);
    nos_puts(": ");
    print_number(result);
    nos_puts("\n");
}

static void uart_stats(void)
{
    struct { uint64_t interrupts; uint32_t received, dropped, ready, reserved; } stats;
    struct nos_message message = {.op = NOS_VFS_DEVICE_STATS, .arg = 1};
    if (nos_rpc(&message) || message.len != sizeof(stats)) return;
    u_memcpy(&stats, message.data, sizeof(stats));
    char digits[21];
    unsigned index = sizeof(digits);
    uint64_t number = stats.interrupts;
    digits[--index] = 0;
    do { digits[--index] = (char)('0' + number % 10); number /= 10; } while (number);
    nos_puts("uart_irq: ");
    nos_puts(digits + index);
    nos_puts("\n");
    shell_report("uart_rx", stats.received);
    shell_report("uart_dropped", stats.dropped);
    shell_report("uart_ready", stats.ready);
}

static int exec_linux(const char *path, const char *const *argv, const char *const *envp)
{
#ifdef NOS_LINUX_COMPAT
    unsigned char random[16];
    for (size_t copied = 0; copied < sizeof(random);) {
        struct nos_message entropy = {.op = NOS_VFS_GETRANDOM, .len = sizeof(random) - copied};
        intptr_t result = nos_rpc(&entropy);
        if (result < 0) return (int)result;
        if (!result || (uintptr_t)result > sizeof(random) - copied || entropy.len != (size_t)result)
            return -NOS_EIO;
        u_memcpy(random + copied, entropy.data, (size_t)result);
        copied += (size_t)result;
    }
    struct nos_exec_request request = {path, argv, envp, 8};
    return (int)nos_syscall(NOS_SYS_EXEC_LINUX, (uintptr_t)&request,
                          (uintptr_t)random, NOS_EXEC_RANDOM_VALID);
#else
    (void)path; (void)argv; (void)envp;
    return -NOS_ENOTSUP;
#endif
}

void shell_run_program(unsigned count, char **words, unsigned linux_abi)
{
    if (!count || count >= NOS_SHELL_WORDS) {
        nos_puts("usage: run /bin/program [args]\n");
        return;
    }
    const char *arguments[NOS_SHELL_WORDS];
    for (unsigned index = 0; index < count; index++) arguments[index] = words[index];
    arguments[count] = 0;
    const char *environment[] = {"PATH=/bin", "NOS=1", "LD_LIBRARY_PATH=/lib", 0};
    int pid = linux_abi ? exec_linux(words[0], arguments, environment) :
              nos_exec(words[0], arguments, environment, 8);
    if (pid < 0) { shell_report("exec", pid); return; }
    intptr_t status = 0;
    intptr_t result = nos_syscall(NOS_SYS_WAIT, (uintptr_t)pid, (uintptr_t)&status, 0);
    shell_report("program", result < 0 ? result : status);
}

static void command_ls(struct nos_shell *shell, unsigned count, char **words)
{
    (void)shell;
    int fd = nos_open(count == 2 ? words[1] : ".", NOS_O_RDONLY);
    if (fd < 0) { shell_report("ls", fd); return; }
    char name[NOS_MSG_DATA];
    int result;
    for (size_t index = 0; (result = nos_readdir(fd, index, name, sizeof(name))) > 0; index++) {
        nos_puts(name);
        nos_puts("\n");
    }
    if (result < 0) shell_report("ls", result);
    result = nos_close(fd);
    if (result < 0) shell_report("close", result);
}

static void command_cat(struct nos_shell *shell, unsigned count, char **words)
{
    (void)shell; (void)count;
    int fd = nos_open(words[1], NOS_O_RDONLY);
    if (fd < 0) { shell_report("cat", fd); return; }
    char buffer[NOS_MSG_DATA];
    intptr_t result;
    while ((result = nos_read(fd, buffer, sizeof(buffer))) > 0) {
        result = shell_write_all(1, buffer, (size_t)result);
        if (result < 0) break;
    }
    if (result < 0) shell_report("cat", result);
    else nos_puts("\n");
    int closed = nos_close(fd);
    if (closed < 0) shell_report("close", closed);
}

static void command_write(struct nos_shell *shell, unsigned count, char **words)
{
    (void)shell;
    int fd = nos_open(words[1], NOS_O_WRONLY | NOS_O_CREAT | NOS_O_TRUNC);
    if (fd < 0) { shell_report("write", fd); return; }
    intptr_t total = 0;
    for (unsigned index = 2; index < count; index++) {
        intptr_t result;
        if (index > 2) {
            result = shell_write_all(fd, " ", 1);
            if (result < 0) { total = result; break; }
            total += result;
        }
        result = shell_write_all(fd, words[index], u_strlen(words[index]));
        if (result < 0) { total = result; break; }
        total += result;
    }
    int closed = nos_close(fd);
    shell_report("write", closed < 0 && total >= 0 ? closed : total);
}

static void command_pwd(struct nos_shell *shell, unsigned count, char **words)
{
    (void)shell; (void)count; (void)words;
    char path[NOS_PATH_MAX];
    intptr_t result = nos_getcwd(path, sizeof(path));
    if (result < 0) shell_report("pwd", result);
    else { nos_puts(path); nos_puts("\n"); }
}

static void command_cd(struct nos_shell *shell, unsigned count, char **words)
{
    (void)shell;
    int result = nos_chdir(count == 2 ? words[1] : "/");
    if (result < 0) shell_report("cd", result);
}

static void command_echo(struct nos_shell *shell, unsigned count, char **words)
{
    (void)shell;
    for (unsigned index = 1; index < count; index++) {
        if (index > 1) nos_puts(" ");
        nos_puts(words[index]);
    }
    nos_puts("\n");
}

static void command_history(struct nos_shell *shell, unsigned count, char **words)
{
    (void)count; (void)words;
    struct nos_shell_editor *editor = &shell->editor;
    for (unsigned offset = editor->history_count; offset; offset--) {
        unsigned index = (editor->history_next + NOS_SHELL_HISTORY - offset) % NOS_SHELL_HISTORY;
        nos_puts(editor->history[index]);
        nos_puts("\n");
    }
}

static void command_restart(struct nos_shell *shell, unsigned count, char **words)
{
    (void)count; (void)words;
    intptr_t result = nos_syscall(NOS_SYS_TASK_CANCEL, shell->service_pid, 0, 0);
    if (result && result != -NOS_ESRCH) { shell_report("restart", result); return; }
    for (unsigned attempt = 0; attempt < 100; attempt++) {
        result = nos_syscall(NOS_SYS_SERVICE_RESTART, 0, 0, 0);
        if (result != -NOS_EBUSY) break;
        nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
    }
    if (result >= 0) shell->service_pid = (unsigned)result;
    shell_report("restart", result);
}

static void command_stats(struct nos_shell *shell, unsigned count, char **words)
{
    (void)shell; (void)count; (void)words;
    struct nos_object_stats stats;
    intptr_t result = nos_syscall(NOS_SYS_OBJECT_STATS, (uintptr_t)&stats, 0, 0);
    if (result < 0) shell_report("stats", result);
    else {
        shell_report("tasks", stats.tasks);
        shell_report("mutexes", stats.mutexes);
        shell_report("endpoints", stats.endpoints);
        shell_report("free_pages", stats.free_pages);
    }
    uart_stats();
}

/* File operations remain in the VFS service; the shell only invokes its ABI. */
static void command_filesystem(struct nos_shell *shell, unsigned count, char **words)
{
    (void)shell;
    int result;
    if (u_streq(words[0], "mkdir")) result = nos_mkdir(words[1]);
    else if (u_streq(words[0], "mv")) result = nos_rename(words[1], words[2]);
    else if (u_streq(words[0], "rm")) result = nos_unlink(words[1]);
    else if (u_streq(words[0], "rmdir")) result = nos_rmdir(words[1]);
    else if (u_streq(words[0], "mount")) result = nos_mount(count == 2 ? words[1] : "/persist", 1);
    else result = nos_sync();
    shell_report(words[0], result);
}

static void command_run(struct nos_shell *shell, unsigned count, char **words)
{
    (void)shell;
    shell_run_program(count - 1, words + 1, u_streq(words[0], "runlinux"));
}

static void command_help(struct nos_shell *shell, unsigned count, char **words);

struct shell_builtin {
    const char *name;
    unsigned minimum, maximum;
    const char *usage;
    void (*execute)(struct nos_shell *, unsigned, char **);
};

static const struct shell_builtin builtins[] = {
    {"help", 1, 1, "help", command_help},
    {"pwd", 1, 1, "pwd", command_pwd},
    {"cd", 1, 2, "cd [path]", command_cd},
    {"echo", 1, NOS_SHELL_WORDS, "echo [args]", command_echo},
    {"history", 1, 1, "history", command_history},
    {"ls", 1, 2, "ls [path]", command_ls},
    {"cat", 2, 2, "cat path", command_cat},
    {"write", 3, NOS_SHELL_WORDS, "write path text [text...]", command_write},
    {"mkdir", 2, 2, "mkdir path", command_filesystem},
    {"mv", 3, 3, "mv old new", command_filesystem},
    {"rm", 2, 2, "rm path", command_filesystem},
    {"rmdir", 2, 2, "rmdir path", command_filesystem},
    {"mount", 1, 2, "mount [path]", command_filesystem},
    {"sync", 1, 1, "sync", command_filesystem},
    {"run", 2, NOS_SHELL_WORDS, "run /bin/program [args]", command_run},
#ifdef NOS_LINUX_COMPAT
    {"runlinux", 2, NOS_SHELL_WORDS, "runlinux /bin/program [args]", command_run},
#endif
    {"stats", 1, 1, "stats", command_stats},
    {"restart", 1, 1, "restart", command_restart}
};

static void command_help(struct nos_shell *shell, unsigned count, char **words)
{
    (void)shell; (void)count; (void)words;
    for (unsigned index = 0; index < sizeof(builtins) / sizeof(builtins[0]); index++) {
        nos_puts(builtins[index].usage);
        nos_puts("\n");
    }
    nos_puts("Quotes: 'text' or \"text\"; backslash escapes the next character.\n"
             "Edit: arrows, Home/End, Delete, Ctrl-A/E/U/K; Ctrl-C cancels input.\n"
             "History: Up/Down (4 entries). Pipelines and expansion are unsupported.\n");
}

void shell_command(struct nos_shell *shell, char *line)
{
    char *words[NOS_SHELL_WORDS + 1];
    int count = nos_shell_parse(line, words, NOS_SHELL_WORDS);
    if (count < 0) {
        nos_puts(count == NOS_SHELL_PARSE_QUOTE ? "parse: unmatched quote\n" :
                 count == NOS_SHELL_PARSE_ESCAPE ? "parse: trailing escape\n" :
                 "parse: too many arguments\n");
        return;
    }
    if (!count) return;
    for (unsigned index = 0; index < sizeof(builtins) / sizeof(builtins[0]); index++) {
        const struct shell_builtin *builtin = &builtins[index];
        if (!u_streq(words[0], builtin->name)) continue;
        if ((unsigned)count < builtin->minimum || (unsigned)count > builtin->maximum) {
            nos_puts("usage: ");
            nos_puts(builtin->usage);
            nos_puts("\n");
        } else builtin->execute(shell, (unsigned)count, words);
        return;
    }
    nos_puts("invalid command; type help\n");
}
