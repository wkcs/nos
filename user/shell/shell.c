/* Copyright (C) 2026 NOS contributors */
#include "commands.h"

static void terminal_emit(void *context, const char *text, size_t length)
{
    (void)context;
    (void)shell_write_all(1, text, length);
}

void nos_shell_run(void)
{
    static struct nos_shell shell;
    nos_shell_editor_init(&shell.editor);
    nos_puts("NOS SHELL READY\n" NOS_SHELL_PROMPT);
    for (;;) {
        unsigned char character;
        intptr_t result = nos_read(0, &character, 1);
        if (result == -NOS_EAGAIN || !result) {
            nos_syscall(NOS_SYS_SLEEP, 1, 0, 0);
            continue;
        }
        if (result < 0) {
            intptr_t restarted = nos_syscall(NOS_SYS_SERVICE_RESTART, 0, 0, 0);
            if (restarted >= 0) {
                shell.service_pid = (unsigned)restarted;
                nos_puts("VFS restarted\n");
            }
            nos_syscall(NOS_SYS_SLEEP, 10, 0, 0);
            continue;
        }
        enum nos_shell_event event = nos_shell_editor_feed(&shell.editor, character,
                                                          terminal_emit, 0);
        if (event == NOS_SHELL_EVENT_LINE) shell_command(&shell, shell.editor.line);
        else if (event == NOS_SHELL_EVENT_TOO_LONG) nos_puts("command too long\n");
        if (event != NOS_SHELL_EVENT_NONE) nos_puts(NOS_SHELL_PROMPT);
    }
}
