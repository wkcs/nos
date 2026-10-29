/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "../../app/shell/pipe.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define DATA_SIZE 65536
static struct shell_stream_pipe streams[5];
static struct shell_stream_pipe *stream = &streams[0];
static char source[DATA_SIZE], dest[DATA_SIZE];
static atomic_int started, finished;
static int writer_result, reader_result;

static void *writer(void *arg)
{
    (void)arg;
    atomic_store(&started, 1);
    writer_result = shell_stream_pipe_write(stream, source, sizeof(source));
    shell_stream_pipe_close_writer(stream);
    atomic_store(&finished, 1);
    return NULL;
}
static void *reader(void *arg)
{
    int n;
    (void)arg;
    atomic_store(&started, 1);
    n = shell_stream_pipe_read(stream, dest, 1);
    reader_result = n;
    return NULL;
}
static void wait_started(void)
{
    const struct timespec pause = { 0, 1000000 };
    while (!atomic_load(&started)) nanosleep(&pause, NULL);
}
static void wait_writer_blocked(void)
{
    const struct timespec pause = { 0, 1000000 };
    for (int i = 0; i < 2000; i++) {
        int waiting;
        mutex_lock(&stream->lock);
        waiting = stream->writer_waiting && stream->count == SHELL_STREAM_PIPE_CAPACITY;
        mutex_unlock(&stream->lock);
        if (waiting) return;
        nanosleep(&pause, NULL);
    }
    assert(!"writer did not block on a full pipe");
}
int main(void)
{
    pthread_t thread;
    size_t total = 0;
    for (size_t i = 0; i < sizeof(source); i++) source[i] = (char)(i % 251);
    assert(shell_stream_pipe_init(stream) == 0);
    assert(pthread_create(&thread, NULL, writer, NULL) == 0);
    wait_started();
    while (total < sizeof(dest)) {
        int n = shell_stream_pipe_read(stream, dest + total, sizeof(dest) - total);
        assert(n > 0);
        total += (size_t)n;
    }
    assert(shell_stream_pipe_read(stream, dest, 1) == 0);
    pthread_join(thread, NULL);
    assert(writer_result == DATA_SIZE && memcmp(source, dest, DATA_SIZE) == 0);
    shell_stream_pipe_close_reader(stream);

    stream = &streams[1];
    assert(shell_stream_pipe_init(stream) == 0);
    atomic_store(&started, 0); atomic_store(&finished, 0);
    assert(pthread_create(&thread, NULL, writer, NULL) == 0);
    wait_started();
    wait_writer_blocked();
    assert(!atomic_load(&finished));
    shell_stream_pipe_close_reader(stream);
    pthread_join(thread, NULL);
    assert(atomic_load(&finished) && writer_result >= 0 && writer_result < DATA_SIZE);
    assert(shell_stream_pipe_write(stream, source, 1) == -EBADF);

    stream = &streams[2];
    assert(shell_stream_pipe_init(stream) == 0);
    atomic_store(&started, 0);
    assert(pthread_create(&thread, NULL, reader, NULL) == 0);
    wait_started();
    shell_stream_pipe_close_writer(stream);
    pthread_join(thread, NULL);
    assert(reader_result == 0);
    shell_stream_pipe_close_reader(stream);
    stream = &streams[3];
    assert(shell_stream_pipe_init(stream) == 0);
    assert(shell_stream_pipe_write(stream, "abc", 3) == 3);
    shell_stream_pipe_close_writer(stream);
    assert(shell_stream_pipe_read(stream, dest, sizeof(dest)) == 3);
    assert(memcmp(dest, "abc", 3) == 0);
    assert(shell_stream_pipe_read(stream, dest, 1) == 0);
    shell_stream_pipe_close_reader(stream);
    stream = &streams[4];
    assert(shell_stream_pipe_init(stream) == 0);
    shell_stream_pipe_close_reader(stream);
    assert(shell_stream_pipe_write(stream, source, 1) == -EPIPE);
    puts("shell streaming pipe checks passed");
    return 0;
}
