/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * Raw TTY byte stream, independent of the underlying transport.
 */

#include <nos/tty.h>
#include <uapi/nos/errno.h>
#include <stddef.h>

int nos_tty_init(struct nos_tty *tty, const struct nos_tty_ops *ops, void *context)
{
    if (tty == NULL || ops == NULL) {
        return -EINVAL;
    }
    tty->ops = ops;
    tty->context = context;
#ifdef CONFIG_TTY_CANONICAL
    tty->mode = 0;
    tty->line_len = 0;
    tty->line_pos = 0;
    tty->line_complete = false;
#endif
    return 0;
}

int nos_tty_set_mode(struct nos_tty *tty, unsigned int flags)
{
    if (tty == NULL || (flags & ~(NOS_TTY_CANONICAL | NOS_TTY_ECHO |
                                   NOS_TTY_ICRNL | NOS_TTY_ECHO_CRLF)) != 0) {
        return -EINVAL;
    }
    if (flags != 0 && !(flags & NOS_TTY_CANONICAL)) {
        return -EINVAL;
    }
#ifdef CONFIG_TTY_CANONICAL
    tty->mode = flags;
    tty->line_len = 0;
    tty->line_pos = 0;
    tty->line_complete = false;
    return 0;
#else
    return flags == 0 ? 0 : -ENOSYS;
#endif
}

#ifdef CONFIG_TTY_CANONICAL
static void tty_echo(struct nos_tty *tty, const unsigned char *buf, unsigned int len)
{
    if ((tty->mode & NOS_TTY_ECHO) && tty->ops->write != NULL) {
        (void)tty->ops->write(tty->context, buf, len);
    }
}

static int tty_read_canonical(struct nos_tty *tty, unsigned char *buf,
                              unsigned int len, bool wait)
{
    while (!tty->line_complete) {
        unsigned char ch;
        int rc = tty->ops->read(tty->context, &ch, 1, wait);

        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            if (tty->line_len == 0) return 0;
            tty->line_complete = true;
            break;
        }
        if (ch == '\r' && (tty->mode & NOS_TTY_ICRNL)) {
            ch = '\n';
        }
        if (ch == '\b' || ch == 0x7f) {
            if (tty->line_len != 0) {
                static const unsigned char erase[] = { '\b', ' ', '\b' };
                tty->line_len--;
                tty_echo(tty, erase, sizeof(erase));
            }
            continue;
        }
        if (ch == '\n') {
            static const unsigned char crlf[] = { '\r', '\n' };
            tty->line[tty->line_len++] = ch;
            tty->line_complete = true;
            if (tty->mode & NOS_TTY_ECHO_CRLF) {
                tty_echo(tty, crlf, sizeof(crlf));
            } else {
                tty_echo(tty, &ch, 1);
            }
        } else if (tty->line_len < CONFIG_TTY_LINE_SIZE - 1) {
            tty->line[tty->line_len++] = ch;
            tty_echo(tty, &ch, 1);
        }
    }
    unsigned int copied = 0;
    while (copied < len && tty->line_pos < tty->line_len) {
        buf[copied++] = tty->line[tty->line_pos++];
    }
    if (tty->line_pos == tty->line_len) {
        tty->line_len = 0;
        tty->line_pos = 0;
        tty->line_complete = false;
    }
    return (int)copied;
}
#endif

int nos_tty_read(struct nos_tty *tty, unsigned char *buf, unsigned int len, bool wait)
{
    if (tty == NULL || (buf == NULL && len != 0)) {
        return -EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    if (tty->ops == NULL || tty->ops->read == NULL) {
        return -ENOSYS;
    }
#ifdef CONFIG_TTY_CANONICAL
    if (tty->mode & NOS_TTY_CANONICAL) {
        return tty_read_canonical(tty, buf, len, wait);
    }
#endif
    return tty->ops->read(tty->context, buf, len, wait);
}

int nos_tty_write(struct nos_tty *tty, const unsigned char *buf, unsigned int len)
{
    if (tty == NULL || (buf == NULL && len != 0)) {
        return -EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    if (tty->ops == NULL || tty->ops->write == NULL) {
        return -ENOSYS;
    }
    return tty->ops->write(tty->context, buf, len);
}
