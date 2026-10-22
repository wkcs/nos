/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_SHELL_COMMANDS_H
#define NOS_SHELL_COMMANDS_H

#include "user.h"
#include "shell.h"

struct nos_shell {
    struct nos_shell_editor editor;
    unsigned service_pid;
};

intptr_t shell_write_all(int fd, const char *buffer, size_t length);
void shell_report(const char *operation, intptr_t result);
void shell_run_program(unsigned count, char **words, unsigned linux_abi);
void shell_command(struct nos_shell *shell, char *line);

#endif
