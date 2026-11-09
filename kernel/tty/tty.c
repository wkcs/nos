/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * Standardized TTY subsystem: raw byte stream, POSIX line discipline,
 * and VFS character device operations.
 */

#include <nos/tty.h>
#include <drivers/serial.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <uapi/nos/errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/**
 * 底层读取适配：优先使用绑定的 device，否则回退至 ops
 */
static int tty_backend_read(struct nos_tty *tty, unsigned char *buf, unsigned int len, bool wait)
{
    if (tty->dev != NULL) {
        ssize_t rc = uart_read(tty->dev, buf, (size_t)len, wait);
        return (int)rc;
    }
    if (tty->ops != NULL && tty->ops->read != NULL) {
        return tty->ops->read(tty->context, buf, len, wait);
    }
    return -ENOSYS;
}

/**
 * 底层写入适配：优先使用绑定的 device，否则回退至 ops
 */
static int tty_backend_write(struct nos_tty *tty, const unsigned char *buf, unsigned int len)
{
    if (tty->dev != NULL) {
        ssize_t rc = uart_write(tty->dev, buf, (size_t)len);
        return (int)rc;
    }
    if (tty->ops != NULL && tty->ops->write != NULL) {
        return tty->ops->write(tty->context, buf, len);
    }
    return -ENOSYS;
}

int nos_tty_init(struct nos_tty *tty, const struct nos_tty_ops *ops, void *context)
{
    if (tty == NULL || ops == NULL) {
        return -EINVAL;
    }
    tty->dev = NULL;
    tty->ops = ops;
    tty->context = context;
    tty->termios.c_iflag = 0;
    tty->termios.c_oflag = 0;
    tty->termios.c_lflag = 0;
    spin_lock_init(&tty->lock);
    nos_wait_queue_init(&tty->read_wait);
#ifdef CONFIG_TTY_CANONICAL
    tty->mode = 0;
    tty->line_len = 0;
    tty->line_pos = 0;
    tty->line_complete = false;
#endif
    return 0;
}

int nos_tty_init_dev(struct nos_tty *tty, const struct device *dev)
{
    if (tty == NULL || dev == NULL) {
        return -EINVAL;
    }
    tty->dev = dev;
    tty->ops = NULL;
    tty->context = NULL;
    tty->termios.c_iflag = 0;
    tty->termios.c_oflag = 0;
    tty->termios.c_lflag = 0;
    spin_lock_init(&tty->lock);
    nos_wait_queue_init(&tty->read_wait);
#ifdef CONFIG_TTY_CANONICAL
    tty->mode = 0;
    tty->line_len = 0;
    tty->line_pos = 0;
    tty->line_complete = false;
#endif
    return 0;
}

int nos_tty_ioctl(struct nos_tty *tty, unsigned int cmd, unsigned long arg)
{
    if (tty == NULL) {
        return -EINVAL;
    }
    switch (cmd) {
    case TCGETS: {
        struct nos_termios *tio = (struct nos_termios *)(uintptr_t)arg;
        if (tio == NULL) {
            return -EINVAL;
        }
        *tio = tty->termios;
        return 0;
    }
    case TCSETS: {
        const struct nos_termios *tio = (const struct nos_termios *)(uintptr_t)arg;
        if (tio == NULL) {
            return -EINVAL;
        }
        tty->termios = *tio;
#ifdef CONFIG_TTY_CANONICAL
        tty->mode = 0;
        if (tty->termios.c_lflag & ICANON) tty->mode |= NOS_TTY_CANONICAL;
        if (tty->termios.c_lflag & ECHO)   tty->mode |= NOS_TTY_ECHO;
        if (tty->termios.c_iflag & ICRNL)  tty->mode |= NOS_TTY_ICRNL;
        if (tty->termios.c_oflag & ONLCR)  tty->mode |= NOS_TTY_ECHO_CRLF;
        tty->line_len = 0;
        tty->line_pos = 0;
        tty->line_complete = false;
#endif
        return 0;
    }
    default:
        return -ENOSYS;
    }
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
    tty->termios.c_iflag = 0;
    tty->termios.c_oflag = 0;
    tty->termios.c_lflag = 0;
    if (flags & NOS_TTY_ICRNL) {
        tty->termios.c_iflag |= ICRNL;
    }
    if (flags & NOS_TTY_ECHO_CRLF) {
        tty->termios.c_oflag |= ONLCR;
    }
    if (flags & NOS_TTY_CANONICAL) {
        tty->termios.c_lflag |= ICANON;
    }
    if (flags & NOS_TTY_ECHO) {
        tty->termios.c_lflag |= (ECHO | ECHOE);
    }
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
    if ((tty->termios.c_lflag & ECHO) && len != 0) {
        (void)tty_backend_write(tty, buf, len);
    }
}

static int tty_read_canonical(struct nos_tty *tty, unsigned char *buf,
                              unsigned int len, bool wait)
{
    while (!tty->line_complete) {
        unsigned char ch;
        int rc = tty_backend_read(tty, &ch, 1, wait);

        if (rc < 0) {
            return rc;
        }
        if (rc == 0) {
            if (tty->line_len == 0) {
                return 0;
            }
            tty->line_complete = true;
            break;
        }

        /* IGNCR: 忽略输入回车 */
        if (ch == '\r' && (tty->termios.c_iflag & IGNCR)) {
            continue;
        }

        /* ICRNL: 回车映射为换行 */
        if (ch == '\r' && (tty->termios.c_iflag & ICRNL)) {
            ch = '\n';
        }

        /* 退格键与删除键 (0x7F / '\b') */
        if (ch == '\b' || ch == 0x7f) {
            if (tty->line_len != 0) {
                tty->line_len--;
                if (tty->termios.c_lflag & ECHOE) {
                    static const unsigned char erase[] = { '\b', ' ', '\b' };
                    tty_echo(tty, erase, sizeof(erase));
                } else {
                    tty_echo(tty, &ch, 1);
                }
            }
            continue;
        }

        /* 换行符完成整行输入 */
        if (ch == '\n') {
            static const unsigned char crlf[] = { '\r', '\n' };
            tty->line[tty->line_len++] = ch;
            tty->line_complete = true;
            if (tty->termios.c_oflag & ONLCR) {
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
    if (tty->dev == NULL && (tty->ops == NULL || tty->ops->read == NULL)) {
        return -ENOSYS;
    }
#ifdef CONFIG_TTY_CANONICAL
    if (tty->termios.c_lflag & ICANON) {
        return tty_read_canonical(tty, buf, len, wait);
    }
#endif
    return tty_backend_read(tty, buf, len, wait);
}

int nos_tty_write(struct nos_tty *tty, const unsigned char *buf, unsigned int len)
{
    if (tty == NULL || (buf == NULL && len != 0)) {
        return -EINVAL;
    }
    if (len == 0) {
        return 0;
    }
    if (tty->dev == NULL && (tty->ops == NULL || tty->ops->write == NULL)) {
        return -ENOSYS;
    }

    if (!(tty->termios.c_oflag & ONLCR)) {
        return tty_backend_write(tty, buf, len);
    }

    /* 当开启 ONLCR 时，将 \n 展开为 \r\n */
    unsigned int written_in = 0;
    while (written_in < len) {
        unsigned char out_chunk[32];
        unsigned int out_len = 0;
        unsigned int chunk_in = 0;

        while (written_in + chunk_in < len && out_len + 2 <= sizeof(out_chunk)) {
            unsigned char c = buf[written_in + chunk_in];
            if (c == '\n') {
                out_chunk[out_len++] = '\r';
                out_chunk[out_len++] = '\n';
                chunk_in++;
                break;
            } else {
                out_chunk[out_len++] = c;
                chunk_in++;
            }
        }

        int rc = tty_backend_write(tty, out_chunk, out_len);
        if (rc < 0) {
            return written_in == 0 ? rc : (int)written_in;
        }
        written_in += chunk_in;
        if ((unsigned int)rc < out_len) {
            break;
        }
    }
    return (int)written_in;
}

bool nos_tty_poll_session(struct nos_tty *tty, uint32_t *generation, bool *non_interactive)
{
    if (tty == NULL) {
        return false;
    }
    if (tty->ops != NULL && tty->ops->poll_session != NULL) {
        return tty->ops->poll_session(tty->context, generation, non_interactive);
    }
    if (generation != NULL) {
        *generation = 1U;
    }
    if (non_interactive != NULL) {
        *non_interactive = false;
    }
    return true;
}

void nos_tty_on_exit(struct nos_tty *tty, int32_t exit_code)
{
    if (tty != NULL && tty->ops != NULL && tty->ops->on_exit != NULL) {
        tty->ops->on_exit(tty->context, exit_code);
    }
}

static int tty_fop_open(struct inode *inode, struct file *file)
{
    if (file == NULL) {
        return -EINVAL;
    }
    if (file->private_data == NULL && inode != NULL) {
        file->private_data = inode->i_private;
    }
    return 0;
}

static int tty_fop_release(struct inode *inode, struct file *file)
{
    (void)inode;
    (void)file;
    return 0;
}

static ssize_t tty_fop_read(struct file *file, char *buf, size_t len, loff_t *pos)
{
    (void)pos;
    if (file == NULL || (buf == NULL && len != 0)) {
        return -EINVAL;
    }
    struct nos_tty *tty = (struct nos_tty *)file->private_data;
    if (tty == NULL) {
        return -ENODEV;
    }
    bool wait = !(file->f_flags & O_NONBLOCK);
    return nos_tty_read(tty, (unsigned char *)buf, (unsigned int)len, wait);
}

static ssize_t tty_fop_write(struct file *file, const char *buf, size_t len, loff_t *pos)
{
    (void)pos;
    if (file == NULL || (buf == NULL && len != 0)) {
        return -EINVAL;
    }
    struct nos_tty *tty = (struct nos_tty *)file->private_data;
    if (tty == NULL) {
        return -ENODEV;
    }
    return nos_tty_write(tty, (const unsigned char *)buf, (unsigned int)len);
}

static int tty_fop_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    if (file == NULL) {
        return -EINVAL;
    }
    struct nos_tty *tty = (struct nos_tty *)file->private_data;
    if (tty == NULL) {
        return -ENODEV;
    }
    return nos_tty_ioctl(tty, cmd, arg);
}

const struct file_operations nos_tty_fops = {
    .open = tty_fop_open,
    .release = tty_fop_release,
    .read = tty_fop_read,
    .write = tty_fop_write,
    .ioctl = tty_fop_ioctl,
};
