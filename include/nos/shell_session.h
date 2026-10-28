/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef NOS_SHELL_SESSION_H
#define NOS_SHELL_SESSION_H

#include <nos/tty.h>
#include <stddef.h>

#define NOS_SHELL_SESSION_API 1
#define NOS_SHELL_FEED_API 1
#define SHELL_SESSION_INPUT_MAX 256

struct dentry;
struct shell_session {
    struct nos_tty *tty;
    struct dentry *cwd;
    char input[SHELL_SESSION_INPUT_MAX + 1];
    size_t input_len;
    size_t text_len;
    unsigned int ansi_state;
    bool after_cr;
    bool overflow;
    int (*writer)(void *context, const char *buf, unsigned int len);
    void *writer_context;
};

void shell_session_init(struct shell_session *session, struct nos_tty *tty,
                        struct dentry *cwd);
void shell_session_reset(struct shell_session *session, struct dentry *cwd);
int shell_session_write(struct shell_session *session, const char *buf, unsigned int len);
void shell_session_feed(struct shell_session *session, unsigned char ch,
                        void (*execute)(struct shell_session *, void *), void *context);
#endif
