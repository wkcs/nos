/** Copyright (C) 2026 Nick Hu */
#ifndef NOS_TEST_KFIFO_H
#define NOS_TEST_KFIFO_H
#include <stddef.h>
struct kfifo {
    unsigned int in;
    unsigned int out;
    unsigned int mask;
    unsigned int esize;
    void *data;
};
unsigned int kfifo_unused(struct kfifo *fifo);
unsigned int kfifo_used(struct kfifo *fifo);
int kfifo_init(struct kfifo *fifo, void *buffer, unsigned int size, size_t esize);
unsigned int kfifo_in(struct kfifo *fifo, const void *buf, unsigned int len);
unsigned int kfifo_out(struct kfifo *fifo, void *buf, unsigned int len);
unsigned int kfifo_out_peek(struct kfifo *fifo, void *buf, unsigned int len);
unsigned int kfifo_skip(struct kfifo *fifo, unsigned int len);
#endif
