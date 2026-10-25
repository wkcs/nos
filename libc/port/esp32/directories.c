/* Copyright (C) 2026 NOS contributors. Newlib directory objects over NOS VFS. */
#include "private.h"
#include <dirent.h>
#include <string.h>

struct directory_node {
    DIR prefix;
    struct directory_node *next;
    unsigned pid;
    int fd;
    size_t index;
    struct dirent entry;
};
static struct directory_node *lookup(DIR *directory) {
    int context = nos_libc_context(__getreent());
    if (context != 1) {
        nos_libc_result(__getreent(), context < 0 ? context : -NOS_ENOSYS);
        return NULL;
    }
    struct libc_state *s = nos_libc_state();
    if (s)
        for (struct directory_node *n = s->directories; n; n = n->next)
            if ((DIR *)n == directory)
                return n;
    errno = EBADF;
    return NULL;
}
DIR *__wrap_opendir(const char *path) {
    int rc = nos_esp_libc_task_enter();
    if (rc < 0 || !nos_libc_state()) {
        nos_libc_result(__getreent(), rc < 0 ? rc : -NOS_ENOSYS);
        return NULL;
    }
    struct libc_state *s = nos_libc_state();
    if (s->directory_count >= NOS_FD_LIMIT) {
        errno = EMFILE;
        return NULL;
    }
    int fd = (int)nos_libc_result(__getreent(), nos_open(path, NOS_O_RDONLY | NOS_O_DIRECTORY));
    if (fd < 0)
        return NULL;
    struct directory_node *n = calloc(1, sizeof(*n));
    if (!n) {
        nos_close(fd);
        return NULL;
    }
    n->prefix.dd_vfs_idx = UINT16_MAX;
    n->pid = s->pid;
    n->fd = fd;
    n->next = s->directories;
    s->directories = n;
    s->directory_count++;
    return (DIR *)n;
}
struct dirent *__wrap_readdir(DIR *directory) {
    struct directory_node *n = lookup(directory);
    if (!n)
        return NULL;
    char name[NOS_PATH_MAX];
    int rc = nos_readdir(n->fd, n->index, name, sizeof(name));
    if (rc <= 0) {
        if (rc)
            nos_libc_result(__getreent(), rc);
        return NULL;
    }
    memset(&n->entry, 0, sizeof(n->entry));
    memcpy(n->entry.d_name, name, strlen(name) + 1);
    n->entry.d_type = DT_UNKNOWN;
    n->index++;
    return &n->entry;
}
int __wrap_readdir_r(DIR *directory, struct dirent *entry, struct dirent **result) {
    if (!entry || !result)
        return EINVAL;
    int saved = errno;
    errno = 0;
    struct dirent *value = __wrap_readdir(directory);
    int error = errno;
    if (value)
        *entry = *value;
    *result = value ? entry : NULL;
    errno = saved;
    return error;
}
static void release(struct libc_state *s, struct directory_node *n, int orphan) {
    struct directory_node **link = &s->directories;
    while (*link && *link != n)
        link = &(*link)->next;
    if (*link)
        *link = n->next;
    s->directory_count--;
    if (!orphan)
        nos_close(n->fd);
    free(n);
}
int __wrap_closedir(DIR *directory) {
    struct directory_node *n = lookup(directory);
    if (!n)
        return -1;
    int rc = nos_close(n->fd);
    release(nos_libc_state(), n, 1);
    return (int)nos_libc_result(__getreent(), rc);
}
long __wrap_telldir(DIR *directory) {
    struct directory_node *n = lookup(directory);
    return n ? (long)n->index : -1;
}
void __wrap_seekdir(DIR *directory, long offset) {
    struct directory_node *n = lookup(directory);
    if (!n)
        return;
    if (offset < 0) {
        errno = EINVAL;
        return;
    }
    n->index = (size_t)offset;
}
void __wrap_rewinddir(DIR *directory) {
    __wrap_seekdir(directory, 0);
}
void nos_libc_directories_cleanup(struct libc_state *s, int orphan) {
    while (s->directories)
        release(s, s->directories, orphan);
}
