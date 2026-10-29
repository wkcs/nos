/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "pipe.h"
#include <kernel/errno.h>
#include <limits.h>
#include <string.h>

int shell_stream_pipe_init(struct shell_stream_pipe *pipe)
{
    if (pipe == NULL) return -EINVAL;
    memset(pipe, 0, sizeof(*pipe));
    if (mutex_init(&pipe->lock) != 0 || sem_init(&pipe->readable, 0) != 0 ||
        sem_init(&pipe->writable, 0) != 0) return -EINVAL;
    return 0;
}

int shell_stream_pipe_read(struct shell_stream_pipe *pipe, char *data, size_t size)
{
    size_t amount, first;
    bool wake;
    if (pipe == NULL || (data == NULL && size != 0)) return -EINVAL;
    if (size == 0) return 0;
    mutex_lock(&pipe->lock);
    while (pipe->count == 0 && !pipe->writer_closed && !pipe->reader_closed) {
        pipe->reader_waiting = true;
        mutex_unlock(&pipe->lock);
        sem_get(&pipe->readable);
        mutex_lock(&pipe->lock);
    }
    if (pipe->reader_closed) {
        mutex_unlock(&pipe->lock);
        return -EBADF;
    }
    if (pipe->count == 0) {
        mutex_unlock(&pipe->lock);
        return 0;
    }
    amount = size < pipe->count ? size : pipe->count;
    first = SHELL_STREAM_PIPE_CAPACITY - pipe->head;
    if (first > amount) first = amount;
    memcpy(data, pipe->data + pipe->head, first);
    memcpy(data + first, pipe->data, amount - first);
    pipe->head = (pipe->head + amount) % SHELL_STREAM_PIPE_CAPACITY;
    pipe->count -= amount;
    wake = pipe->writer_waiting;
    pipe->writer_waiting = false;
    mutex_unlock(&pipe->lock);
    if (wake) sem_send_one(&pipe->writable);
    return (int)amount;
}

int shell_stream_pipe_write(struct shell_stream_pipe *pipe, const char *data, size_t size)
{
    size_t total = 0;
    if (pipe == NULL || (data == NULL && size != 0)) return -EINVAL;
    if (size > INT_MAX) size = INT_MAX;
    while (total < size) {
        size_t tail, amount, first;
        bool wake;
        mutex_lock(&pipe->lock);
        while (pipe->count == SHELL_STREAM_PIPE_CAPACITY &&
               !pipe->reader_closed && !pipe->writer_closed) {
            pipe->writer_waiting = true;
            mutex_unlock(&pipe->lock);
            sem_get(&pipe->writable);
            mutex_lock(&pipe->lock);
        }
        if (pipe->reader_closed || pipe->writer_closed) {
            int error = pipe->writer_closed ? -EBADF : -EPIPE;
            mutex_unlock(&pipe->lock);
            return total != 0 ? (int)total : error;
        }
        amount = size - total;
        if (amount > SHELL_STREAM_PIPE_CAPACITY - pipe->count)
            amount = SHELL_STREAM_PIPE_CAPACITY - pipe->count;
        tail = (pipe->head + pipe->count) % SHELL_STREAM_PIPE_CAPACITY;
        first = SHELL_STREAM_PIPE_CAPACITY - tail;
        if (first > amount) first = amount;
        memcpy(pipe->data + tail, data + total, first);
        memcpy(pipe->data, data + total + first, amount - first);
        pipe->count += amount;
        total += amount;
        wake = pipe->reader_waiting;
        pipe->reader_waiting = false;
        mutex_unlock(&pipe->lock);
        if (wake) sem_send_one(&pipe->readable);
    }
    return (int)total;
}

void shell_stream_pipe_close_reader(struct shell_stream_pipe *pipe)
{
    bool wake_reader, wake_writer;
    if (pipe == NULL) return;
    mutex_lock(&pipe->lock);
    pipe->reader_closed = true;
    wake_reader = pipe->reader_waiting;
    wake_writer = pipe->writer_waiting;
    pipe->reader_waiting = false;
    pipe->writer_waiting = false;
    mutex_unlock(&pipe->lock);
    if (wake_reader) sem_send_one(&pipe->readable);
    if (wake_writer) sem_send_one(&pipe->writable);
}

void shell_stream_pipe_close_writer(struct shell_stream_pipe *pipe)
{
    bool wake_reader, wake_writer;
    if (pipe == NULL) return;
    mutex_lock(&pipe->lock);
    pipe->writer_closed = true;
    wake_reader = pipe->reader_waiting;
    wake_writer = pipe->writer_waiting;
    pipe->reader_waiting = false;
    pipe->writer_waiting = false;
    mutex_unlock(&pipe->lock);
    if (wake_reader) sem_send_one(&pipe->readable);
    if (wake_writer) sem_send_one(&pipe->writable);
}
