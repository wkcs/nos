/* Copyright (C) 2026 NOS contributors */
#include "init.h"
#include "shell.h"
#include "../shell/commands.h"

int nos_exec(const char *path, const char *const *argv, const char *const *envp, unsigned priority)
{
    struct nos_exec_request request = {path, argv, envp, priority};
    return (int)nos_syscall(NOS_SYS_EXEC_SPAWN, (uintptr_t)&request, 0, 0);
}

void nos_init(uintptr_t argument)
{
    (void)argument;
    nos_puts("NOS NORMAL BOOT: supervised VFS and independent ELF applications\n");
    char *boot_arguments[] = {"/bin/hello", "boot"};
    shell_run_program(2, boot_arguments, 0);
    nos_shell_run();
}
