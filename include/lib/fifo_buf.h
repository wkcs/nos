/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __FIFO_BUF_H__
#define __FIFO_BUF_H__

#include <kernel/kernel.h>

struct fifo_buf_t {
    addr_t buf_start;
    addr_t buf_end;
    addr_t read;
    addr_t write;
    size_t free_size;
    size_t buf_size;
};

#define FIFO_BUF_INIT(start, end) { \
    .buf_start = (addr_t)start,  \
    .buf_end = (addr_t)end,  \
    .read = (addr_t)start,  \
    .write = (addr_t)start,  \
    .free_size = (addr_t)end - (addr_t)start,  \
    .buf_size = (addr_t)end - (addr_t)start,  \
}

#define FIFO_BUF(name, start, end) \
	struct fifo_buf_t name = FIFO_BUF_INIT(start, end)

size_t read_fifo_buf_size(struct fifo_buf_t *fifo, uint8_t *buf, size_t len);
size_t read_fifo_buf_size_test(struct fifo_buf_t *fifo, uint8_t *buf, size_t len);
size_t write_fifo_buf_size(struct fifo_buf_t *fifo, uint8_t *buf, size_t len);


#endif