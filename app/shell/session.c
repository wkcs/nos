/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <nos/shell_session.h>
#include <uapi/nos/errno.h>
#include <string.h>

void shell_session_reset(struct shell_session *session, struct dentry *cwd)
{
    memset(session->input, 0, sizeof(session->input));
    session->input_len = 0;
    session->text_len = 0;
    session->ansi_state = 0;
    session->after_cr = false;
    session->overflow = false;
    session->cwd = cwd;
}

void shell_session_init(struct shell_session *session, struct nos_tty *tty,
                        struct dentry *cwd)
{
    session->tty = tty;
    session->writer = NULL;
    session->writer_context = NULL;
    shell_session_reset(session, cwd);
}

int shell_session_write(struct shell_session *session, const char *buf, unsigned int len)
{
    if (session == NULL) return -EINVAL;
    if (session->writer != NULL) return session->writer(session->writer_context, buf, len);
    if (session->tty == NULL) return -EINVAL;
    return nos_tty_write(session->tty, (const unsigned char *)buf, len);
}

static void shell_session_line(struct shell_session *session,
                               void (*execute)(struct shell_session *, void *), void *context)
{
    shell_session_write(session, "\r\n", 2);
    if (session->overflow) {
        static const char error[] = "shell: line too long\r\n";
        shell_session_write(session, error, sizeof(error) - 1U);
    } else if (execute != NULL) {
        execute(session, context);
    }
    session->overflow = false;
    memset(session->input, 0, sizeof(session->input));
    session->input_len = session->text_len = 0;
    shell_session_write(session, "nos# ", 5);
}

static size_t utf8_prev(const char *buf, size_t pos)
{
    if (pos == 0U) return 0U;
    pos--;
    while (pos > 0U && (((unsigned char)buf[pos] & 0xC0U) == 0x80U)) {
        pos--;
    }
    return pos;
}

static size_t utf8_next(const char *buf, size_t pos, size_t len)
{
    if (pos >= len) return len;
    pos++;
    while (pos < len && (((unsigned char)buf[pos] & 0xC0U) == 0x80U)) {
        pos++;
    }
    return pos;
}

void shell_session_feed(struct shell_session *session, unsigned char ch,
                        void (*execute)(struct shell_session *, void *), void *context)
{
    size_t i;
    if (session == NULL) return;
    if (session->ansi_state == 1) {
        session->ansi_state = ch == '[' ? 2 : 0;
        return;
    }
    if (session->ansi_state == 2) {
        session->ansi_state = 0;
        if (ch == 'D' && session->input_len > 0) {
            session->input_len = utf8_prev(session->input, session->input_len);
            shell_session_write(session, "\033[D", 3);
        } else if (ch == 'C' && session->input_len < session->text_len) {
            session->input_len = utf8_next(session->input, session->input_len,
                                           session->text_len);
            shell_session_write(session, "\033[C", 3);
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
        for (i = session->input_len; i < session->text_len; i++)
            shell_session_write(session, "\033[D", 3);
        session->after_cr = false;
    } else if (ch == '\r' || ch == '\n') {
        if (ch == '\n' && session->after_cr) {
            session->after_cr = false;
            return;
        }
        shell_session_line(session, execute, context);
        session->after_cr = ch == '\r';
    } else if (ch == 0x03) {
        memset(session->input, 0, sizeof(session->input));
        session->input_len = session->text_len = 0;
        session->after_cr = false;
        session->overflow = false;
        shell_session_write(session, "^C\r\nnos# ", 9);
    } else if (ch == 0x1b) {
        session->ansi_state = 1;
    } else if ((ch == 0x08 || ch == 0x7f) && session->input_len > 0) {
        size_t prev = utf8_prev(session->input, session->input_len);
        size_t removed = session->input_len - prev;
        session->input_len = prev;
        for (i = session->input_len; i + removed <= session->text_len; i++)
            session->input[i] = session->input[i + removed];
        session->text_len -= removed;
        session->input[session->text_len] = '\0';
        shell_session_write(session, "\b", 1);
        shell_session_write(session, session->input + session->input_len,
                            (unsigned int)(session->text_len - session->input_len));
        shell_session_write(session, " ", 1);
        for (i = session->input_len; i <= session->text_len; i++)
            shell_session_write(session, "\033[D", 3);
    }
}
