/**
 * Copyright (C) 2024-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[NSHELL]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/task.h>
#include <kernel/errno.h>
#include <kernel/sleep.h>
#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/spinlock.h>
#include <kernel/mm.h>
#include <string.h>
#include <kernel/sem.h>
#include <kernel/console.h>

#define ANSI_CMD_PARAM_MAX 10
struct ansi_cmd {
    bool is_escape_sequence;
    bool has_param;
    char cmd;
    char params[ANSI_CMD_PARAM_MAX];
};

#define SHELL_CMD_STR_MAX 256
struct shell_cmd {
    char buf[SHELL_CMD_STR_MAX + 1];
    size_t index;
};

struct nshell {
    struct task_struct *task;
    struct shell_cmd cmd;
    struct ansi_cmd ansi_cmd;
};

#define isdigit(c) ((unsigned)((c) - '0') < 10)

static bool isprint(char ch)
{
    return (ch >= 0x20 && ch <= 0x7e);
}

static char nshell_parse_ansi_cmd(struct ansi_cmd *cmd)
{
    char ch;
    size_t index = 0;
    bool start = false;

    memset(cmd, 0, sizeof(struct ansi_cmd));
    cmd->is_escape_sequence = false;
    cmd->has_param = false;
    cmd->is_escape_sequence = true;

    while (true) {
        ch = console_getc();
        console_putc(ch);
        if (ch != '[') {
            break;
        }
        start = true;
    }
    if (!start) {
        return ch;
    }

    while (true) {
        ch = console_getc();
        console_putc(ch);
        if (isdigit(ch) || ch == ';' || ch == '?') {
            cmd->params[index++] = ch;
            if (index >= ANSI_CMD_PARAM_MAX) {
                pr_err("too many params\r\n");
                break;
            }
            continue;
        }
        break;
    }

    if (index > 0) {
        cmd->has_param = true;
    }
    // 读取命令
    cmd->cmd = console_getc();
    console_putc(cmd->cmd);

    return 0;
}

void nshell_exec_ansi_cmd(struct nshell *shell) {
    const struct ansi_cmd *cmd = &shell->ansi_cmd;
    struct shell_cmd *shell_cmd = &shell->cmd;

    if (!cmd->is_escape_sequence) {
        pr_err("Not an escape sequence.\r\n");
        return;
    }

    if (cmd->has_param) {
        pr_debug("Parameters: %s\r\n", cmd->params);
    }

    switch (cmd->cmd) {
        case 'A':  // 上移
            pr_info("Move cursor up.\r\n");
            break;
        case 'B':  // 下移
            pr_info("Move cursor down.\r\n");
            break;
        case 'C':  // 右移
            if (shell_cmd->index < SHELL_CMD_STR_MAX - 1)
                shell_cmd->index++;
            size_t len = strlen(shell_cmd->buf);
            if (shell_cmd->index > len)
                shell_cmd->index = len;
            pr_info("Move cursor right.\r\n");
            break;
        case 'D':  // 左移
            if (shell_cmd->index > 0)
                shell_cmd->index--;
            pr_info("Move cursor left.\r\n");
            break;
        case 'J':  // 清除屏幕
            if (cmd->params[0] == '2') {
                pr_info("Clear entire screen.\r\n");
            } else {
                pr_info("Clear from current position to end of screen.\r\n");
            }
            break;
        case 'K':  // 清除当前行
            if (cmd->params[0] == '2') {
                pr_info("Clear entire line.\r\n");
            } else {
                pr_info("Clear from current position to end of line.\r\n");
            }
            break;
        case 'm':  // 文本样式
            pr_info("Set text style.\r\n");
            break;
        default:
            pr_err("Unknown cmd: %c\r\n", cmd->cmd);
    }
}

static void nshell_exec_cmd(struct nshell *shell)
{

}

static void nshell_new_line(struct nshell *shell)
{
    console_putc('\r');
    console_putc('\n');
    nshell_exec_cmd(shell);
    memset(shell->cmd.buf, 0, SHELL_CMD_STR_MAX);
    shell->cmd.index = 0;
}

static void nshell_task_entry(void* parameter)
{
    struct nshell *shell = parameter;
    char ch;
    size_t len = 0;
    bool ansi_cmd = false;
    bool newline = false;

    memset(shell->cmd.buf, 0, SHELL_CMD_STR_MAX);
    shell->cmd.index = 0;

    while (true) {
        if (ansi_cmd) {
            ansi_cmd = false;
            ch = nshell_parse_ansi_cmd(&shell->ansi_cmd);
            if (ch == 0) {
                nshell_exec_ansi_cmd(shell);
                continue;
            }
        } else {
            ch = console_getc();
        }

        if (isprint(ch)) {
            if (shell->cmd.index >= len) {
                shell->cmd.index == len;
            } else {
                for (int i = len; i > shell->cmd.index; i--) {
                    shell->cmd.buf[i] = shell->cmd.buf[i - 1];
                }
            }
            shell->cmd.buf[shell->cmd.index++] = ch;
            len++;
            console_putc(ch);
            if (len >= SHELL_CMD_STR_MAX) {
                nshell_new_line(shell);
                len = 0;
            }
        } else if (ch == '\r') {
            nshell_new_line(shell);
            len = 0;
            newline = true;
        } else if (ch == '\n') {
            if (newline) {
                newline = false;
                continue;
            }
            nshell_new_line(shell);
            len = 0;
            newline = false;
        } else if (ch == '\033') {
            ansi_cmd = true;
            console_putc(ch);
        }
    }
}

static int nshell_init(void)
{
    struct nshell *shell;

    shell = kzalloc(sizeof(struct nshell), GFP_KERNEL);
    if (shell == NULL) {
        pr_err("alloc nshell buf error\r\n");
        return -ENOMEM;
    }

    shell->task = task_create("nshell",
        nshell_task_entry, shell, 5, 512, 10, NULL);
    if (shell->task == NULL) {
        pr_fatal("creat nshell task err\r\n");

        return -EINVAL;
    }
    task_ready(shell->task);

    return 0;
}
task_init(nshell_init);
