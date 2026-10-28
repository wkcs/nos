/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <fs/vfs.h>
#include "../../fs/vfs_fd.h"

static pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t signal_cond = PTHREAD_COND_INITIALIZER;
static int acquired, proceed, released;

static int release_file(struct inode *inode, struct file *file)
{
    (void)inode;
    assert(file->marker == 42);
    released++;
    return 0;
}

static const struct file_operations operations = { .release = release_file };

static void *reader(void *arg)
{
    int fd = *(int *)arg;
    struct file *file = vfs_fd_get(fd);
    assert(file != NULL && file->marker == 42);
    pthread_mutex_lock(&gate);
    acquired = 1;
    pthread_cond_broadcast(&signal_cond);
    while (!proceed) pthread_cond_wait(&signal_cond, &gate);
    pthread_mutex_unlock(&gate);
    assert(file->marker == 42);
    vfs_fd_put(file);
    return NULL;
}

int main(void)
{
    struct file *file = calloc(1, sizeof(*file));
    pthread_t thread;
    int fd = vfs_fd_reserve();
    assert(fd >= 0);
    file->marker = 42;
    file->f_op = &operations;
    vfs_fd_publish(fd, file);
    assert(pthread_create(&thread, NULL, reader, &fd) == 0);
    pthread_mutex_lock(&gate);
    while (!acquired) pthread_cond_wait(&signal_cond, &gate);
    assert(vfs_fd_close(fd) == 0);
    assert(released == 0);
    assert(vfs_fd_get(fd) == NULL);
    assert(vfs_fd_reserve() == fd);
    {
        struct file *replacement = calloc(1, sizeof(*replacement));
        assert(replacement != NULL);
        replacement->marker = 42;
        replacement->f_op = &operations;
        vfs_fd_publish(fd, replacement);
        assert(vfs_fd_close(fd) == 0);
        assert(released == 1);
    }
    proceed = 1;
    pthread_cond_broadcast(&signal_cond);
    pthread_mutex_unlock(&gate);
    pthread_join(thread, NULL);
    assert(released == 2);
    assert(vfs_fd_close(fd) < 0);
    return 0;
}
