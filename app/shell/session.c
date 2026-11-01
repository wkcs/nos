/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <nos/shell_session.h>
#include <uapi/nos/errno.h>
#include <string.h>

#define SHELL_DEFAULT_PROMPT "nos# "

static size_t utf8_prev(const char *buf, size_t pos)
{
    if (pos == 0U) {
        return 0U;
    }
    pos--;
    while (pos > 0U && (((unsigned char)buf[pos] & 0xC0U) == 0x80U)) {
        pos--;
    }
    return pos;
}

static size_t utf8_next(const char *buf, size_t pos, size_t len)
{
    if (pos >= len) {
        return len;
    }
    pos++;
    while (pos < len && (((unsigned char)buf[pos] & 0xC0U) == 0x80U)) {
        pos++;
    }
    return pos;
}

static size_t utf8_codepoints(const char *buf, size_t start, size_t end)
{
    size_t count = 0U;
    size_t pos = start;

    while (pos < end) {
        pos = utf8_next(buf, pos, end);
        count++;
    }
    return count;
}

static void write_cursor_left(struct shell_session *session, size_t count)
{
    size_t i;

    for (i = 0U; i < count; i++) {
        shell_session_write(session, "\033[D", 3U);
    }
}

static void write_cursor_right(struct shell_session *session, size_t count)
{
    size_t i;

    for (i = 0U; i < count; i++) {
        shell_session_write(session, "\033[C", 3U);
    }
}

const char *shell_session_get_prompt(const struct shell_session *session)
{
    if (session != NULL && session->prompt[0] != '\0') {
        return session->prompt;
    }
    return SHELL_DEFAULT_PROMPT;
}

void shell_session_set_prompt(struct shell_session *session, const char *prompt)
{
    if (session == NULL) {
        return;
    }
    if (prompt == NULL || prompt[0] == '\0') {
        strcpy(session->prompt, SHELL_DEFAULT_PROMPT);
        return;
    }
    strncpy(session->prompt, prompt, sizeof(session->prompt) - 1U);
    session->prompt[sizeof(session->prompt) - 1U] = '\0';
}

void shell_session_set_completer(struct shell_session *session,
                                 shell_complete_fn_t completer, void *context)
{
    if (session == NULL) {
        return;
    }
    session->completer = completer;
    session->completer_context = context;
}

void shell_session_reset(struct shell_session *session, struct dentry *cwd)
{
    if (session == NULL) {
        return;
    }
    memset(session->input, 0, sizeof(session->input));
    memset(session->saved_input, 0, sizeof(session->saved_input));
    session->input_len = 0;
    session->text_len = 0;
    session->ansi_state = 0;
    session->ansi_param = 0;
    session->after_cr = false;
    session->overflow = false;
    shell_session_history_clear(session);
    session->cwd = cwd;
    if (session->prompt[0] == '\0') {
        strcpy(session->prompt, SHELL_DEFAULT_PROMPT);
    }
}

void shell_session_init(struct shell_session *session, struct nos_tty *tty,
                        struct dentry *cwd,
                        char history[SHELL_SESSION_HISTORY_MAX][SHELL_SESSION_INPUT_MAX + 1])
{
    if (session == NULL) {
        return;
    }
    memset(session, 0, sizeof(*session));
    session->history = history;
    session->tty = tty;
    session->writer = NULL;
    session->writer_context = NULL;
    session->completer = NULL;
    session->completer_context = NULL;
    strcpy(session->prompt, SHELL_DEFAULT_PROMPT);
    shell_session_reset(session, cwd);
}

int shell_session_write(struct shell_session *session, const char *buf, unsigned int len)
{
    if (session == NULL) return -EINVAL;
    if (session->writer != NULL) return session->writer(session->writer_context, buf, len);
    if (session->tty == NULL) return -EINVAL;
    return nos_tty_write(session->tty, (const unsigned char *)buf, len);
}

void shell_session_history_clear(struct shell_session *session)
{
    if (session == NULL) {
        return;
    }
    if (session->history != NULL)
        memset(session->history, 0, SHELL_SESSION_HISTORY_MAX * sizeof(*session->history));
    session->history_count = 0U;
    session->history_head = 0U;
    session->history_index = -1;
}

unsigned int shell_session_history_count(const struct shell_session *session)
{
    return session != NULL ? session->history_count : 0U;
}

const char *shell_session_history_get(const struct shell_session *session,
                                      unsigned int index)
{
    unsigned int slot;

    if (session == NULL || session->history == NULL || index >= session->history_count) {
        return NULL;
    }
    if (session->history_count < SHELL_SESSION_HISTORY_MAX) {
        slot = index;
    } else {
        slot = (session->history_head + index) % SHELL_SESSION_HISTORY_MAX;
    }
    return session->history[slot];
}

void shell_session_history_add(struct shell_session *session, const char *line)
{
    const char *p = line;
    const char *last;
    unsigned int slot;

    if (session == NULL || session->history == NULL || line == NULL) {
        return;
    }
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p == '\0') {
        return;
    }
    if (session->history_count > 0U) {
        last = shell_session_history_get(session, session->history_count - 1U);
        if (last != NULL && strcmp(last, line) == 0) {
            return;
        }
    }
    if (session->history_count < SHELL_SESSION_HISTORY_MAX) {
        slot = session->history_count++;
    } else {
        slot = session->history_head;
        session->history_head = (session->history_head + 1U) % SHELL_SESSION_HISTORY_MAX;
    }
    strncpy(session->history[slot], line, SHELL_SESSION_INPUT_MAX);
    session->history[slot][SHELL_SESSION_INPUT_MAX] = '\0';
}

void shell_session_replace_input(struct shell_session *session,
                                 const char *text, size_t cursor_pos)
{
    size_t old_cursor_chars;
    size_t old_total_chars;
    size_t new_len;
    size_t new_total_chars;
    size_t tail_chars;
    size_t i;

    if (session == NULL || text == NULL) {
        return;
    }
    old_cursor_chars = utf8_codepoints(session->input, 0U, session->input_len);
    old_total_chars = utf8_codepoints(session->input, 0U, session->text_len);

    new_len = strlen(text);
    if (new_len > SHELL_SESSION_INPUT_MAX) {
        new_len = SHELL_SESSION_INPUT_MAX;
    }
    if (cursor_pos > new_len) {
        cursor_pos = new_len;
    }

    write_cursor_left(session, old_cursor_chars);
    if (new_len != 0U && text != session->input) {
        memmove(session->input, text, new_len);
    }
    session->input[new_len] = '\0';
    session->text_len = new_len;
    session->input_len = cursor_pos;

    if (new_len > 0U) {
        shell_session_write(session, session->input, (unsigned int)new_len);
    }
    new_total_chars = utf8_codepoints(session->input, 0U, session->text_len);
    if (old_total_chars > new_total_chars) {
        size_t extra = old_total_chars - new_total_chars;
        for (i = 0U; i < extra; i++) {
            shell_session_write(session, " ", 1U);
        }
        write_cursor_left(session, extra);
    }
    tail_chars = utf8_codepoints(session->input, session->input_len, session->text_len);
    write_cursor_left(session, tail_chars);
}

void shell_session_redraw(struct shell_session *session)
{
    const char *prompt;
    size_t tail_chars;

    if (session == NULL) {
        return;
    }
    prompt = shell_session_get_prompt(session);
    shell_session_write(session, prompt, (unsigned int)strlen(prompt));
    if (session->text_len > 0U) {
        shell_session_write(session, session->input, (unsigned int)session->text_len);
    }
    tail_chars = utf8_codepoints(session->input, session->input_len, session->text_len);
    write_cursor_left(session, tail_chars);
}

static void shell_session_line(struct shell_session *session,
                               void (*execute)(struct shell_session *, void *), void *context)
{
    const char *prompt;

    shell_session_write(session, "\n", 1);
    if (session->overflow) {
        static const char error[] = "shell: line too long\n";
        shell_session_write(session, error, sizeof(error) - 1U);
    } else {
        if (session->text_len > 0U) {
            shell_session_history_add(session, session->input);
        }
        if (execute != NULL) {
            execute(session, context);
        }
    }
    session->overflow = false;
    session->history_index = -1;
    memset(session->input, 0, sizeof(session->input));
    memset(session->saved_input, 0, sizeof(session->saved_input));
    session->input_len = session->text_len = 0;
    prompt = shell_session_get_prompt(session);
    shell_session_write(session, prompt, (unsigned int)strlen(prompt));
}

static void shell_session_delete_at_cursor(struct shell_session *session)
{
    size_t next;
    size_t removed;
    size_t i;
    size_t tail_chars;

    if (session->input_len >= session->text_len) {
        return;
    }
    next = utf8_next(session->input, session->input_len, session->text_len);
    removed = next - session->input_len;
    for (i = session->input_len; i + removed <= session->text_len; i++) {
        session->input[i] = session->input[i + removed];
    }
    session->text_len -= removed;
    session->input[session->text_len] = '\0';
    shell_session_write(session, session->input + session->input_len,
                        (unsigned int)(session->text_len - session->input_len));
    shell_session_write(session, " ", 1U);
    tail_chars = utf8_codepoints(session->input, session->input_len, session->text_len);
    write_cursor_left(session, tail_chars + 1U);
}

static void shell_session_move_home(struct shell_session *session)
{
    if (session->input_len > 0U) {
        size_t chars = utf8_codepoints(session->input, 0U, session->input_len);
        session->input_len = 0U;
        write_cursor_left(session, chars);
    }
}

static void shell_session_move_end(struct shell_session *session)
{
    if (session->input_len < session->text_len) {
        size_t chars = utf8_codepoints(session->input, session->input_len, session->text_len);
        session->input_len = session->text_len;
        write_cursor_right(session, chars);
    }
}

static void shell_session_history_up(struct shell_session *session)
{
    const char *entry;

    if (session->history_count == 0U) {
        return;
    }
    if (session->history_index < 0) {
        memcpy(session->saved_input, session->input, session->text_len + 1U);
        session->history_index = (int)session->history_count - 1;
    } else if (session->history_index > 0) {
        session->history_index--;
    } else {
        return;
    }
    entry = shell_session_history_get(session, (unsigned int)session->history_index);
    if (entry != NULL) {
        shell_session_replace_input(session, entry, strlen(entry));
    }
}

static void shell_session_history_down(struct shell_session *session)
{
    const char *entry;

    if (session->history_index < 0) {
        return;
    }
    if ((unsigned int)(session->history_index + 1) < session->history_count) {
        session->history_index++;
        entry = shell_session_history_get(session, (unsigned int)session->history_index);
        if (entry != NULL) {
            shell_session_replace_input(session, entry, strlen(entry));
        }
    } else {
        char draft[SHELL_SESSION_INPUT_MAX + 1];
        session->history_index = -1;
        strcpy(draft, session->saved_input);
        shell_session_replace_input(session, draft, strlen(draft));
    }
}

void shell_session_feed(struct shell_session *session, unsigned char ch,
                        void (*execute)(struct shell_session *, void *), void *context)
{
    size_t i;

    if (session == NULL) return;
    if (session->ansi_state == 1) {
        if (ch == '[') {
            session->ansi_state = 2;
            session->ansi_param = 0;
        } else if (ch == 'O') {
            session->ansi_state = 3;
        } else {
            session->ansi_state = 0;
        }
        return;
    }
    if (session->ansi_state == 2 || session->ansi_state == 3) {
        if (session->ansi_state == 2 && ch >= '0' && ch <= '9') {
            session->ansi_param = session->ansi_param * 10U + (unsigned int)(ch - '0');
            session->ansi_state = 4;
            return;
        }
        session->ansi_state = 0;
        if (ch == 'A') {
            shell_session_history_up(session);
        } else if (ch == 'B') {
            shell_session_history_down(session);
        } else if (ch == 'D' && session->input_len > 0) {
            session->input_len = utf8_prev(session->input, session->input_len);
            shell_session_write(session, "\033[D", 3);
        } else if (ch == 'C' && session->input_len < session->text_len) {
            session->input_len = utf8_next(session->input, session->input_len,
                                           session->text_len);
            shell_session_write(session, "\033[C", 3);
        } else if (ch == 'H') {
            shell_session_move_home(session);
        } else if (ch == 'F') {
            shell_session_move_end(session);
        }
        return;
    }
    if (session->ansi_state == 4) {
        if (ch >= '0' && ch <= '9') {
            session->ansi_param = session->ansi_param * 10U + (unsigned int)(ch - '0');
            return;
        }
        session->ansi_state = 0;
        if (ch == '~') {
            if (session->ansi_param == 1U || session->ansi_param == 7U) {
                shell_session_move_home(session);
            } else if (session->ansi_param == 3U) {
                shell_session_delete_at_cursor(session);
            } else if (session->ansi_param == 4U || session->ansi_param == 8U) {
                shell_session_move_end(session);
            }
        }
        return;
    }
    if ((ch >= 0x20 && ch <= 0x7e) || ch >= 0x80) {
        if (session->text_len >= SHELL_SESSION_INPUT_MAX) {
            session->overflow = true;
            return;
        }
        for (i = session->text_len; i > session->input_len; i--)
            session->input[i] = session->input[i - 1];
        session->input[session->input_len++] = (char)ch;
        session->text_len++;
        session->input[session->text_len] = '\0';
        shell_session_write(session, session->input + session->input_len - 1,
                            (unsigned int)(session->text_len - session->input_len + 1));
        if (session->input_len < session->text_len) {
            write_cursor_left(session,
                              utf8_codepoints(session->input, session->input_len,
                                              session->text_len));
        }
        session->after_cr = false;
    } else if (ch == '\r' || ch == '\n') {
        if (ch == '\n' && session->after_cr) {
            session->after_cr = false;
            return;
        }
        shell_session_line(session, execute, context);
        session->after_cr = ch == '\r';
    } else if (ch == '\t') {
        if (session->completer != NULL) {
            session->completer(session, session->completer_context);
        }
    } else if (ch == 0x01) { /* Ctrl+A: Home */
        shell_session_move_home(session);
    } else if (ch == 0x02) { /* Ctrl+B: Left */
        if (session->input_len > 0) {
            session->input_len = utf8_prev(session->input, session->input_len);
            shell_session_write(session, "\033[D", 3);
        }
    } else if (ch == 0x03) {
        const char *prompt = shell_session_get_prompt(session);
        memset(session->input, 0, sizeof(session->input));
        memset(session->saved_input, 0, sizeof(session->saved_input));
        session->input_len = session->text_len = 0;
        session->after_cr = false;
        session->overflow = false;
        session->history_index = -1;
        shell_session_write(session, "^C\n", 3);
        shell_session_write(session, prompt, (unsigned int)strlen(prompt));
    } else if (ch == 0x04) { /* Ctrl+D: Delete char at cursor */
        shell_session_delete_at_cursor(session);
    } else if (ch == 0x05) { /* Ctrl+E: End */
        shell_session_move_end(session);
    } else if (ch == 0x06) { /* Ctrl+F: Right */
        if (session->input_len < session->text_len) {
            session->input_len = utf8_next(session->input, session->input_len,
                                           session->text_len);
            shell_session_write(session, "\033[C", 3);
        }
    } else if (ch == 0x0b) { /* Ctrl+K: Kill to end of line */
        if (session->input_len < session->text_len) {
            char buf[SHELL_SESSION_INPUT_MAX + 1];
            memcpy(buf, session->input, session->input_len);
            buf[session->input_len] = '\0';
            shell_session_replace_input(session, buf, session->input_len);
        }
    } else if (ch == 0x0c) { /* Ctrl+L: Clear screen & redraw */
        shell_session_write(session, "\033[2J\033[H", 7);
        shell_session_redraw(session);
    } else if (ch == 0x0e) { /* Ctrl+N: History down */
        shell_session_history_down(session);
    } else if (ch == 0x10) { /* Ctrl+P: History up */
        shell_session_history_up(session);
    } else if (ch == 0x15) { /* Ctrl+U: Kill to start of line */
        if (session->input_len > 0U) {
            char buf[SHELL_SESSION_INPUT_MAX + 1];
            strcpy(buf, session->input + session->input_len);
            shell_session_replace_input(session, buf, 0U);
        }
    } else if (ch == 0x17) { /* Ctrl+W: Delete word before cursor */
        if (session->input_len > 0U) {
            size_t pos = session->input_len;
            char buf[SHELL_SESSION_INPUT_MAX + 1];
            while (pos > 0U && (session->input[pos - 1U] == ' ' ||
                                session->input[pos - 1U] == '\t')) {
                pos--;
            }
            while (pos > 0U && session->input[pos - 1U] != ' ' &&
                   session->input[pos - 1U] != '\t') {
                pos = utf8_prev(session->input, pos);
            }
            memcpy(buf, session->input, pos);
            strcpy(buf + pos, session->input + session->input_len);
            shell_session_replace_input(session, buf, pos);
        }
    } else if (ch == 0x1b) {
        session->ansi_state = 1;
    } else if ((ch == 0x08 || ch == 0x7f) && session->input_len > 0) {
        size_t prev = utf8_prev(session->input, session->input_len);
        size_t removed = session->input_len - prev;
        size_t tail_chars;
        session->input_len = prev;
        for (i = session->input_len; i + removed <= session->text_len; i++)
            session->input[i] = session->input[i + removed];
        session->text_len -= removed;
        session->input[session->text_len] = '\0';
        shell_session_write(session, "\b", 1);
        shell_session_write(session, session->input + session->input_len,
                            (unsigned int)(session->text_len - session->input_len));
        shell_session_write(session, " ", 1);
        tail_chars = utf8_codepoints(session->input, session->input_len, session->text_len);
        write_cursor_left(session, tail_chars + 1U);
    }
}
