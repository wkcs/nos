/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * Terminal byte-stream interface. A TTY instance has one reader; callers
 * serialize mode changes with reads. Output serialization belongs to transport.
 */

#ifndef NOS_TTY_H
#define NOS_TTY_H

#include <autocfg.h>
#include <stdbool.h>
#include <stdint.h>

#define NOS_TTY_CANONICAL_API 1
#define NOS_TTY_CANONICAL (1U << 0)
#define NOS_TTY_ECHO      (1U << 1)
#define NOS_TTY_ICRNL     (1U << 2)
#define NOS_TTY_ECHO_CRLF (1U << 3)

#ifdef CONFIG_TTY_CANONICAL
#ifndef CONFIG_TTY_LINE_SIZE
#define CONFIG_TTY_LINE_SIZE 128
#endif
#endif

struct nos_tty_ops {
    int (*read)(void *context, unsigned char *buf, unsigned int len, bool wait);
    int (*write)(void *context, const unsigned char *buf, unsigned int len);
    /* 可选会话生命周期钩子（用于按需连接的远端 TTY 前端，如 NLK Shell） */
    bool (*poll_session)(void *context, uint32_t *generation, bool *non_interactive);
    void (*on_exit)(void *context, int32_t exit_code);
};

struct nos_tty {
    const struct nos_tty_ops *ops;
    void *context;
#ifdef CONFIG_TTY_CANONICAL
    unsigned int mode;
    unsigned int line_len;
    unsigned int line_pos;
    bool line_complete;
    unsigned char line[CONFIG_TTY_LINE_SIZE];
#endif
};

int nos_tty_init(struct nos_tty *tty, const struct nos_tty_ops *ops, void *context);
int nos_tty_set_mode(struct nos_tty *tty, unsigned int flags);
int nos_tty_read(struct nos_tty *tty, unsigned char *buf, unsigned int len, bool wait);
int nos_tty_write(struct nos_tty *tty, const unsigned char *buf, unsigned int len);
bool nos_tty_poll_session(struct nos_tty *tty, uint32_t *generation, bool *non_interactive);
void nos_tty_on_exit(struct nos_tty *tty, int32_t exit_code);

#ifdef CONFIG_TTY
struct nos_tty *nos_console_tty(void);
#endif

#endif
