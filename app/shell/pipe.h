/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#ifndef NOS_SHELL_PIPE_H
#define NOS_SHELL_PIPE_H
#include <kernel/mutex.h>
#include <nos/sem.h>
#include <nos/timeout.h>
#include <stdbool.h>
#include <stddef.h>

#define SHELL_STREAM_PIPE_CAPACITY 256

/* One reader and one writer; keep the object alive until both have stopped. */
struct shell_stream_pipe {
    struct mutex lock;
    struct nos_sem readable;
    struct nos_sem writable;
    size_t head;
    size_t count;
    bool reader_closed;
    bool writer_closed;
    bool reader_waiting;
    bool writer_waiting;
    char data[SHELL_STREAM_PIPE_CAPACITY];
};
int shell_stream_pipe_init(struct shell_stream_pipe *pipe);
int shell_stream_pipe_read(struct shell_stream_pipe *pipe, char *data, size_t size);
int shell_stream_pipe_write(struct shell_stream_pipe *pipe, const char *data, size_t size);
void shell_stream_pipe_close_reader(struct shell_stream_pipe *pipe);
void shell_stream_pipe_close_writer(struct shell_stream_pipe *pipe);
#endif
