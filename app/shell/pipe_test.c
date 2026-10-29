/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include "pipe.h"
#include <kernel/errno.h>
#include <kernel/init.h>
#include <kernel/printk.h>
#include <kernel/sleep.h>
#include <kernel/task.h>

#define PIPE_TEST_SIZE 8192U

static struct shell_stream_pipe test_pipe;
static struct shell_stream_pipe closed_pipe;
static unsigned int test_done;
static unsigned int test_failed;

static void pipe_test_complete(void)
{
    if (__atomic_add_fetch(&test_done, 1U, __ATOMIC_ACQ_REL) == 2U) {
        if (__atomic_load_n(&test_failed, __ATOMIC_RELAXED))
            pr_err("shell stream pipe FAILED\r\n");
        else
            pr_info("shell stream pipe OK\r\n");
    }
}

static void pipe_test_writer(void *context)
{
    unsigned int offset;
    char block[64];
    (void)context;
    for (offset = 0; offset < PIPE_TEST_SIZE; offset += sizeof(block)) {
        unsigned int i;
        for (i = 0; i < sizeof(block); i++) block[i] = (char)((offset + i) % 251U);
        if (shell_stream_pipe_write(&test_pipe, block, sizeof(block)) != sizeof(block)) {
            __atomic_store_n(&test_failed, 1U, __ATOMIC_RELAXED);
            break;
        }
    }
    shell_stream_pipe_close_writer(&test_pipe);
    for (offset = 0; offset < PIPE_TEST_SIZE; offset += sizeof(block)) {
        int rc = shell_stream_pipe_write(&closed_pipe, block, sizeof(block));
        if (rc == -EPIPE) break;
        if (rc < 0 || rc > (int)sizeof(block)) {
            __atomic_store_n(&test_failed, 1U, __ATOMIC_RELAXED);
            break;
        }
    }
    if (offset >= PIPE_TEST_SIZE) __atomic_store_n(&test_failed, 1U, __ATOMIC_RELAXED);
    shell_stream_pipe_close_writer(&closed_pipe);
    pipe_test_complete();
}

static void pipe_test_reader(void *context)
{
    unsigned int offset = 0;
    char block[71];
    int n;
    (void)context;
    msleep(20);
    while ((n = shell_stream_pipe_read(&test_pipe, block, sizeof(block))) > 0) {
        int i;
        for (i = 0; i < n; i++) {
            if ((unsigned char)block[i] != (offset + (unsigned int)i) % 251U)
                __atomic_store_n(&test_failed, 1U, __ATOMIC_RELAXED);
        }
        offset += (unsigned int)n;
    }
    if (n != 0 || offset != PIPE_TEST_SIZE) __atomic_store_n(&test_failed, 1U, __ATOMIC_RELAXED);
    shell_stream_pipe_close_reader(&test_pipe);
    msleep(20);
    shell_stream_pipe_close_reader(&closed_pipe);
    pipe_test_complete();
}

static int pipe_test_start(void)
{
    struct task_struct *writer, *reader;
    if (shell_stream_pipe_init(&test_pipe) != 0 || shell_stream_pipe_init(&closed_pipe) != 0) return -EINVAL;
    writer = task_create("pipe_test_writer", pipe_test_writer, NULL, 5, 1024, 10, NULL);
    reader = task_create("pipe_test_reader", pipe_test_reader, NULL, 5, 1024, 10, NULL);
    if (writer == NULL || reader == NULL) {
        if (writer != NULL) task_del(writer);
        if (reader != NULL) task_del(reader);
        pr_err("shell stream pipe FAILED\r\n");
        return -ENOMEM;
    }
    task_ready(writer);
    task_ready(reader);
    return 0;
}
task_init(pipe_test_start);
