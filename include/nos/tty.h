/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * Terminal byte-stream interface and POSIX termios line discipline.
 */

#ifndef NOS_TTY_H
#define NOS_TTY_H

#include <autocfg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <kernel/spinlock.h>
#include <nos/wait.h>

/* POSIX 线路规程标志位定义 */
#define ICRNL   (1U << 0)  /* 输入: 将 CR 转换为 NL */
#define IGNCR   (1U << 1)  /* 输入: 忽略 CR */
#define ONLCR   (1U << 2)  /* 输出: 将 NL 转换为 CR-NL */
#define ICANON  (1U << 3)  /* 规范模式 (行缓冲与行编辑) */
#define ECHO    (1U << 4)  /* 回显输入字符 */
#define ECHOE   (1U << 5)  /* 退格擦除回显 (BS-Space-BS) */

#define TCGETS  0x5401     /* 获取 termios 属性 */
#define TCSETS  0x5402     /* 设置 termios 属性 */

/* 兼容旧 TTY 模式宏 */
#define NOS_TTY_CANONICAL_API 1
#define NOS_TTY_CANONICAL ICANON
#define NOS_TTY_ECHO      ECHO
#define NOS_TTY_ICRNL     ICRNL
#define NOS_TTY_ECHO_CRLF ONLCR

#ifdef CONFIG_TTY_CANONICAL
#ifndef CONFIG_TTY_LINE_SIZE
#define CONFIG_TTY_LINE_SIZE 128
#endif
#endif

/* POSIX termios 终端属性结构体 */
struct nos_termios {
    uint32_t c_iflag;      /* 输入模式: ICRNL, IGNCR */
    uint32_t c_oflag;      /* 输出模式: ONLCR */
    uint32_t c_lflag;      /* 本地模式: ICANON, ECHO, ECHOE */
};

/* 前向声明 */
struct device;
struct file;
struct inode;
struct file_operations;

/* TTY 底层操作钩子 (用于虚拟终端、NLK、测试) */
struct nos_tty_ops {
    int (*read)(void *context, unsigned char *buf, unsigned int len, bool wait);
    int (*write)(void *context, const unsigned char *buf, unsigned int len);
    /* 可选会话生命周期钩子（用于按需连接的远端 TTY 前端，如 NLK Shell） */
    bool (*poll_session)(void *context, uint32_t *generation, bool *non_interactive);
    void (*on_exit)(void *context, int32_t exit_code);
};

/* TTY 核心对象 */
struct nos_tty {
    const struct device       *dev;        /* 绑定的硬件设备 (如 UART) */
    const struct nos_tty_ops  *ops;        /* 绑定的回调操作集 */
    void                      *context;    /* 回调上下文 */
    struct nos_termios         termios;    /* 终端属性配置 */
    spinlock_t                 lock;       /* 终端并发自旋锁 */
    struct nos_wait_queue      read_wait;  /* 读就绪等待队列 */
#ifdef CONFIG_TTY_CANONICAL
    unsigned int               mode;       /* 兼容旧接口模式位 */
    unsigned int               line_len;
    unsigned int               line_pos;
    bool                       line_complete;
    unsigned char              line[CONFIG_TTY_LINE_SIZE];
#endif
};

int nos_tty_init(struct nos_tty *tty, const struct nos_tty_ops *ops, void *context);
int nos_tty_init_dev(struct nos_tty *tty, const struct device *dev);
int nos_tty_set_mode(struct nos_tty *tty, unsigned int flags);
int nos_tty_ioctl(struct nos_tty *tty, unsigned int cmd, unsigned long arg);
int nos_tty_read(struct nos_tty *tty, unsigned char *buf, unsigned int len, bool wait);
int nos_tty_write(struct nos_tty *tty, const unsigned char *buf, unsigned int len);
bool nos_tty_poll_session(struct nos_tty *tty, uint32_t *generation, bool *non_interactive);
void nos_tty_on_exit(struct nos_tty *tty, int32_t exit_code);

/* 标准 VFS 字符设备文件操作表 */
extern const struct file_operations nos_tty_fops;

#ifdef CONFIG_TTY
struct nos_tty *nos_console_tty(void);
#endif

#endif /* NOS_TTY_H */
