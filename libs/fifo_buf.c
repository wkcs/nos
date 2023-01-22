/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <wk/irq.h>
#include <lib/string.h>
#include <lib/fifo_buf.h>
#include <wk/cpu.h>

static size_t __read_fifo_buf_size(struct fifo_buf_t *fifo, uint8_t *buf, size_t len, bool test)
{
    register size_t log_size = len;

    if (len > fifo->buf_size - fifo->free_size)
        return 0;

    if (fifo->read >= fifo->write && log_size > fifo->buf_end - fifo->read) {
        memcpy((void *)buf, (void *)fifo->read, fifo->buf_end - fifo->read);
        log_size = log_size + fifo->read - fifo->buf_end;
        memcpy((void *)((addr_t)buf + fifo->buf_end - fifo->read), (void *)fifo->buf_start, log_size);
        if (!test) {
            fifo->read = fifo->buf_start + log_size;
            if (fifo->read >= fifo->buf_end)
                fifo->read = fifo->buf_start;
            fifo->free_size += len;
        }

        return len;
    } else {
        memcpy((void *)buf, (void *)fifo->read, log_size);
        if (!test) {
            fifo->read += log_size;
            if (fifo->read >= fifo->buf_end)
                fifo->read = fifo->buf_start;
            fifo->free_size += log_size;
        }

        return log_size;
    }
}

size_t read_fifo_buf_size(struct fifo_buf_t *fifo, uint8_t *buf, size_t len)
{
    register addr_t level;
    register size_t size;

    level = disable_irq_save();

    size = __read_fifo_buf_size(fifo, buf, len, false);

    enable_irq_save(level);

    return size;
}

size_t read_fifo_buf_size_test(struct fifo_buf_t *fifo, uint8_t *buf, size_t len)
{
    register addr_t level;
    register size_t size;

    level = disable_irq_save();

    size = __read_fifo_buf_size(fifo, buf, len, true);

    enable_irq_save(level);

    return size;
}

static size_t __write_fifo_buf_size(struct fifo_buf_t *fifo, uint8_t *buf, size_t len)
{
    register size_t log_size = len;

    if (log_size > fifo->free_size)
        return 0;

    if (fifo->write >= fifo->read && fifo->buf_end - fifo->write < log_size) {
        memcpy((void *)fifo->write, (void *)buf, fifo->buf_end - fifo->write);
        log_size = log_size + fifo->write - fifo->buf_end;
        memcpy((void *)fifo->buf_start, (void *)(buf + fifo->buf_end - fifo->write), log_size);
        fifo->write = fifo->buf_start + log_size;
        if(fifo->write >= fifo->buf_end)
            fifo->write = fifo->buf_start;
        fifo->free_size -= len;

        return len;
    } else {
            memcpy((void *)fifo->write, (void *)buf, len);
            fifo->write += log_size;
            if(fifo->write >= fifo->buf_end)
                fifo->write = fifo->buf_start;
            fifo->free_size -= log_size;

            return log_size;
    }
}

size_t write_fifo_buf_size(struct fifo_buf_t *fifo, uint8_t *buf, size_t len)
{
    register addr_t level;
    register size_t size;

    level = disable_irq_save();

    size = __write_fifo_buf_size(fifo, buf, len);

    enable_irq_save(level);

    return size;
}
