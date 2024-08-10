/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __KFIFO_H__
#define __KFIFO_H__

#include <kernel/kernel.h>
#include <kernel/mm.h>

struct kfifo {
    unsigned int in;
    unsigned int out;
    unsigned int mask;
    unsigned int esize;
    void *data;
};

unsigned int kfifo_unused(struct kfifo *fifo);
unsigned int kfifo_used(struct kfifo *fifo);
int kfifo_alloc(struct kfifo *fifo, unsigned int size, size_t esize, gfp_t gfp_mask);
void kfifo_free(struct kfifo *fifo);
int kfifo_init(struct kfifo *fifo, void *buffer, unsigned int size, size_t esize);
unsigned int kfifo_in(struct kfifo *fifo, const void *buf, unsigned int len);
unsigned int kfifo_out_peek(struct kfifo *fifo, void *buf, unsigned int len);
unsigned int kfifo_out(struct kfifo *fifo, void *buf, unsigned int len);

#endif /* __KFIFO_H__ */