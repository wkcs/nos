/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef NOS_SHELL_SESSION_H
#define NOS_SHELL_SESSION_H

#include <nos/tty.h>
#include <stdbool.h>
#include <stddef.h>

#define NOS_SHELL_SESSION_API 1
#define NOS_SHELL_FEED_API 1
#define NOS_SHELL_HISTORY_API 1
#define NOS_SHELL_COMPLETE_API 1
#define SHELL_SESSION_INPUT_MAX 256
#define SHELL_SESSION_HISTORY_MAX 8
#define SHELL_SESSION_PROMPT_MAX 32

struct dentry;
struct shell_session;

typedef void (*shell_execute_fn_t)(struct shell_session *session, void *context);
typedef void (*shell_complete_fn_t)(struct shell_session *session, void *context);

struct shell_session {
    struct nos_tty *tty;
    struct dentry *cwd;
    char input[SHELL_SESSION_INPUT_MAX + 1];
    size_t input_len;
    size_t text_len;
    unsigned int ansi_state;
    unsigned int ansi_param;
    bool after_cr;
    bool overflow;
    char prompt[SHELL_SESSION_PROMPT_MAX];
    char (*history)[SHELL_SESSION_INPUT_MAX + 1];
    char saved_input[SHELL_SESSION_INPUT_MAX + 1];
    unsigned int history_count;
    unsigned int history_head;
    int history_index;
    int (*writer)(void *context, const char *buf, unsigned int len);
    void *writer_context;
    shell_complete_fn_t completer;
    void *completer_context;
};

/* History storage is caller-owned and must outlive the session; NULL disables it. */
void shell_session_init(struct shell_session *session, struct nos_tty *tty,
                        struct dentry *cwd,
                        char history[SHELL_SESSION_HISTORY_MAX][SHELL_SESSION_INPUT_MAX + 1]);
void shell_session_reset(struct shell_session *session, struct dentry *cwd);
void shell_session_set_prompt(struct shell_session *session, const char *prompt);
const char *shell_session_get_prompt(const struct shell_session *session);
void shell_session_set_completer(struct shell_session *session,
                                 shell_complete_fn_t completer, void *context);
int shell_session_write(struct shell_session *session, const char *buf, unsigned int len);
void shell_session_replace_input(struct shell_session *session,
                                 const char *text, size_t cursor_pos);
void shell_session_redraw(struct shell_session *session);
void shell_session_history_add(struct shell_session *session, const char *line);
unsigned int shell_session_history_count(const struct shell_session *session);
const char *shell_session_history_get(const struct shell_session *session,
                                      unsigned int index);
void shell_session_history_clear(struct shell_session *session);
void shell_session_feed(struct shell_session *session, unsigned char ch,
                        void (*execute)(struct shell_session *, void *), void *context);
int nshell_spawn(struct nos_tty *tty, const char *name, bool show_banner);
#endif
