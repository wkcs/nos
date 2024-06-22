/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/printk.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/console.h>
#include <kernel/spinlock.h>
#include <lib/vsprintf.h>
#include <string.h>
#include <lib/kfifo.h>

static struct kfifo g_log_fifo;
static char *log_buf[CONFIG_LOG_FIFO_BUF_SIZE];
static SPINLOCK(log_buf_lock);

void kernel_log_init(void)
{
    int rc;

    rc = kfifo_init(&g_log_fifo, log_buf, CONFIG_LOG_FIFO_BUF_SIZE, 1);
    if (rc < 0) {
        return;
    }
}

unsigned int kernel_log_write(const void *buf, unsigned int len)
{
    unsigned int rc = 0;

#ifdef CONFIG_UART_DMA
    if (g_log_fifo.data == NULL || buf == NULL) {
        return 0;
    }

    spin_lock_irq(&log_buf_lock);
    rc = kfifo_in(&g_log_fifo, buf, len);
    spin_unlock_irq(&log_buf_lock);

    console_send_log();
#else
    console_write(buf, len);
#endif

    return rc;
}

unsigned int kernel_log_read(void *buf, unsigned int len)
{
    unsigned int rc;

    if (g_log_fifo.data == NULL || buf == NULL) {
        return 0;
    }

    spin_lock_irq(&log_buf_lock);
    rc = kfifo_out(&g_log_fifo, buf, len);
    spin_unlock_irq(&log_buf_lock);

    return rc;
}
