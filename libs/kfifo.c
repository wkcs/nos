/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <lib/kfifo.h>
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <string.h>

/*
 * internal helper to calculate the unused elements in a fifo
 */
unsigned int kfifo_unused(struct kfifo *fifo)
{
    return (fifo->mask + 1) - (fifo->in - fifo->out);
}

unsigned int kfifo_used(struct kfifo *fifo)
{
    return fifo->in - fifo->out;
}

int kfifo_alloc(struct kfifo *fifo, unsigned int size, size_t esize, gfp_t gfp_mask)
{
    /*
     * round up to the next power of 2, since our 'let the indices
     * wrap' technique works only in this case.
     */
    if (!is_power_of_2(size)) {
        // size = rounddown_pow_of_two(size);
        return -EINVAL;
    }

    fifo->in = 0;
    fifo->out = 0;
    fifo->esize = esize;

    if (size < 2) {
        fifo->data = NULL;
        fifo->mask = 0;
        return -EINVAL;
    }

    fifo->data = kmalloc(esize * size, gfp_mask);

    if (!fifo->data) {
        fifo->mask = 0;
        return -ENOMEM;
    }
    fifo->mask = size - 1;

    return 0;
}

void kfifo_free(struct kfifo *fifo)
{
    kfree(fifo->data);
    fifo->in = 0;
    fifo->out = 0;
    fifo->esize = 0;
    fifo->data = NULL;
    fifo->mask = 0;
}

int kfifo_init(struct kfifo *fifo, void *buffer, unsigned int size, size_t esize)
{
    size /= esize;

    if (!is_power_of_2(size)) {
        // size = rounddown_pow_of_two(size);
        return -EINVAL;
    }

    fifo->in = 0;
    fifo->out = 0;
    fifo->esize = esize;
    fifo->data = buffer;

    if (size < 2) {
        fifo->mask = 0;
        return -EINVAL;
    }
    fifo->mask = size - 1;

    return 0;
}

static void kfifo_copy_in(struct kfifo *fifo, const void *src, unsigned int len, unsigned int off)
{
    unsigned int size = fifo->mask + 1;
    unsigned int esize = fifo->esize;
    unsigned int l;

    off &= fifo->mask;
    if (esize != 1) {
        off *= esize;
        size *= esize;
        len *= esize;
    }
    l = min(len, size - off);

    memcpy(fifo->data + off, src, l);
    memcpy(fifo->data, src + l, len - l);
    /*
     * make sure that the data in the fifo is up to date before
     * incrementing the fifo->in index counter
     */
    smp_wmb();
}

unsigned int kfifo_in(struct kfifo *fifo, const void *buf, unsigned int len)
{
    unsigned int l;

    l = kfifo_unused(fifo);
    if (len > l)
        len = l;

    kfifo_copy_in(fifo, buf, len, fifo->in);
    fifo->in += len;
    return len;
}

static void kfifo_copy_out(struct kfifo *fifo, void *dst, unsigned int len, unsigned int off)
{
    unsigned int size = fifo->mask + 1;
    unsigned int esize = fifo->esize;
    unsigned int l;

    off &= fifo->mask;
    if (esize != 1) {
        off *= esize;
        size *= esize;
        len *= esize;
    }
    l = min(len, size - off);

    memcpy(dst, fifo->data + off, l);
    memcpy(dst + l, fifo->data, len - l);
    /*
     * make sure that the data is copied before
     * incrementing the fifo->out index counter
     */
    smp_wmb();
}

unsigned int kfifo_out_peek(struct kfifo *fifo, void *buf, unsigned int len)
{
    unsigned int l;

    l = fifo->in - fifo->out;
    if (len > l)
        len = l;

    kfifo_copy_out(fifo, buf, len, fifo->out);
    return len;
}

unsigned int kfifo_out(struct kfifo *fifo, void *buf, unsigned int len)
{
    len = kfifo_out_peek(fifo, buf, len);
    fifo->out += len;
    return len;
}
