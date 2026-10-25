/* Copyright (C) 2026 NOS contributors. Task-owned Newlib FILE lifecycle. */
#include "private.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

struct stream_node {
    struct stream_node *next;
    struct libc_state *state;
    FILE *file;
    int fd, orphan;
};
struct libc_state *nos_libc_states[NOS_TASKS];
static portMUX_TYPE registry_lock = portMUX_INITIALIZER_UNLOCKED;
extern FILE *__sfp(struct _reent *);
extern FILE *__real__fopen_r(struct _reent *, const char *, const char *);
extern FILE *__real__fdopen_r(struct _reent *, int, const char *);
extern FILE *__real__freopen_r(struct _reent *, const char *, const char *, FILE *);
extern int __real__fclose_r(struct _reent *, FILE *);

struct libc_state *nos_libc_state(void) {
    int pid = nos_libc_pid();
    if (pid < 0)
        return NULL;
    struct libc_state *s = nos_libc_states[(unsigned)pid % NOS_TASKS];
    return s && s->pid == (unsigned)pid ? s : NULL;
}
static int authorized(struct _reent *r, struct stream_node *node) {
    if (node->orphan || r != __getreent() || nos_libc_pid() != (int)node->state->pid) {
        r->_errno = EBADF;
        return 0;
    }
    int context = nos_libc_context(r);
    if (context == 1)
        return 1;
    nos_libc_result(r, context < 0 ? context : -NOS_EDEADLK);
    return 0;
}
static _READ_WRITE_RETURN_TYPE stream_read(struct _reent *r, void *cookie, char *data,
                                           _READ_WRITE_BUFSIZE_TYPE size) {
    struct stream_node *node = cookie;
    return authorized(r, node) ? nos_libc_result(r, nos_posix_read(node->fd, data, size)) : -1;
}
static _READ_WRITE_RETURN_TYPE stream_write(struct _reent *r, void *cookie, const char *data,
                                            _READ_WRITE_BUFSIZE_TYPE size) {
    struct stream_node *node = cookie;
    if (node->orphan || r != __getreent() || nos_libc_pid() != (int)node->state->pid) {
        r->_errno = EBADF;
        return -1;
    }
    return nos_libc_write(r, node->fd, data, size);
}
static _fpos_t stream_seek(struct _reent *r, void *cookie, _fpos_t offset, int whence) {
    struct stream_node *node = cookie;
    return authorized(r, node) ? nos_libc_result(r, nos_seek(node->fd, offset, whence)) : -1;
}
static int stream_close(struct _reent *r, void *cookie) {
    struct stream_node *node = cookie;
    if (!node->orphan && !authorized(r, node))
        return -1;
    int result = node->orphan ? 0 : (int)nos_libc_result(r, nos_close(node->fd));
    portENTER_CRITICAL(&registry_lock);
    struct stream_node **link = &node->state->streams;
    while (*link && *link != node)
        link = &(*link)->next;
    if (*link)
        *link = node->next;
    node->state->stream_count--;
    portEXIT_CRITICAL(&registry_lock);
    free(node);
    return result;
}
/* Return a pointer only for the current owner. Foreign nodes can disappear
 * immediately after the lock is dropped, so callers only receive -1. */
static int find_stream(FILE *file, struct stream_node **owned) {
    int pid = nos_libc_pid(), result = 0;
    *owned = NULL;
    portENTER_CRITICAL(&registry_lock);
    for (unsigned i = 0; i < NOS_TASKS; i++) {
        struct libc_state *s = nos_libc_states[i];
        if (s)
            for (struct stream_node *n = s->streams; n; n = n->next)
                if (n->file == file) {
                    result = s->pid == (unsigned)pid ? 1 : -1;
                    if (result == 1)
                        *owned = n;
                    goto out;
                }
    }
out:
    portEXIT_CRITICAL(&registry_lock);
    return result;
}
static void attach(FILE *file, struct stream_node *node) {
    file->_cookie = node;
    file->_read = stream_read;
    file->_write = stream_write;
    file->_seek = stream_seek;
    file->_close = stream_close;
}
static int adopt(struct libc_state *s, FILE *file, int fd) {
    /* Bound FILE aliases as well as underlying NOS descriptors. */
    if (s->stream_count >= NOS_FD_LIMIT + 3) {
        errno = EMFILE;
        return -1;
    }
    struct stream_node *node = calloc(1, sizeof(*node));
    if (!node)
        return -1;
    portENTER_CRITICAL(&registry_lock);
    *node = (struct stream_node){.next = s->streams, .state = s, .file = file, .fd = fd};
    s->streams = node;
    s->stream_count++;
    portEXIT_CRITICAL(&registry_lock);
    attach(file, node);
    return 0;
}
static FILE *standard_stream(struct libc_state *s, int fd) {
    FILE *file = __sfp(s->reent);
    if (!file)
        return NULL;
    file->_flags = (fd ? __SWR : __SRD) | __SNBF;
    file->_file = fd;
    file->_close = NULL;
#ifdef _REENT_SMALL
    file->_data = s->reent;
#endif
    if (adopt(s, file, fd)) {
        int saved = errno;
        __real__fclose_r(s->reent, file);
        errno = saved;
        return NULL;
    }
    return file;
}
int nos_esp_libc_task_enter(void) {
    struct _reent *r = __getreent();
    int context = nos_libc_context(r);
    if (context != 1)
        return context < 0 ? context : 0;
    unsigned pid = nos_libc_pid(), slot = pid % NOS_TASKS;
    if (nos_libc_states[slot])
        return nos_libc_states[slot]->pid == pid ? 0 : -NOS_EBUSY;
    struct libc_state *s = calloc(1, sizeof(*s));
    if (!s)
        return -NOS_ENOMEM;
    s->pid = pid;
    s->reent = r;
    portENTER_CRITICAL(&registry_lock);
    nos_libc_states[slot] = s;
    portEXIT_CRITICAL(&registry_lock);
    FILE *streams[3] = {0};
    for (unsigned i = 0; i < 3; i++) {
        streams[i] = standard_stream(s, i);
        if (!streams[i]) {
            nos_esp_libc_task_reclaim(pid, r);
            return -NOS_ENOMEM;
        }
    }
    _REENT_STDIN(r) = streams[0];
    _REENT_STDOUT(r) = streams[1];
    _REENT_STDERR(r) = streams[2];
    return 0;
}
static void reset_standard_streams(struct _reent *r) {
    _REENT_STDIN(r) = _REENT_STDIN(_GLOBAL_REENT);
    _REENT_STDOUT(r) = _REENT_STDOUT(_GLOBAL_REENT);
    _REENT_STDERR(r) = _REENT_STDERR(_GLOBAL_REENT);
}
static void cleanup(struct libc_state *s, int orphan) {
    nos_libc_directories_cleanup(s, orphan);
    while (s->streams) {
        struct stream_node *node = s->streams;
        node->orphan = orphan;
        /* Forced/native EXIT discards buffered output, like _exit(). Never
         * issue the dead process's close/write as the idle/deleting task. */
        __real__fclose_r(s->reent, node->file);
    }
    reset_standard_streams(s->reent);
    portENTER_CRITICAL(&registry_lock);
    nos_libc_states[s->pid % NOS_TASKS] = NULL;
    portEXIT_CRITICAL(&registry_lock);
    free(s);
}
void nos_esp_libc_task_exit(void) {
    struct libc_state *s = nos_libc_state();
    if (s && nos_libc_context(__getreent()) == 1)
        cleanup(s, 0);
}
void nos_esp_libc_task_reclaim(unsigned pid, struct _reent *r) {
    struct libc_state *s = nos_libc_states[pid % NOS_TASKS];
    if (s && s->pid == pid)
        cleanup(s, 1);
    reset_standard_streams(r);
}
static FILE *adopt_result(struct _reent *r, FILE *file, int close_on_failure) {
    if (!file || file->_file < 0 || file->_file >= (int)NOS_FD_LIMIT || nos_libc_context(r) != 1)
        return file;
    struct libc_state *s = nos_libc_state();
    struct stream_node *existing;
    int found = find_stream(file, &existing);
    if (found < 0) {
        r->_errno = EBADF;
        return NULL;
    }
    if (found) {
        existing->fd = file->_file;
        attach(file, existing);
        return file;
    }
    if (!s || adopt(s, file, file->_file)) {
        int saved = errno ? errno : ENOMEM;
        if (!close_on_failure)
            file->_close = NULL;
        __real__fclose_r(r, file);
        r->_errno = saved;
        return NULL;
    }
    return file;
}
FILE *__wrap__fopen_r(struct _reent *r, const char *path, const char *mode) {
    int rc = nos_esp_libc_task_enter();
    if (rc < 0) {
        nos_libc_result(r, rc);
        return NULL;
    }
    return adopt_result(r, __real__fopen_r(r, path, mode), 1);
}
FILE *__wrap__fdopen_r(struct _reent *r, int fd, const char *mode) {
    int rc = nos_esp_libc_task_enter();
    if (rc < 0) {
        nos_libc_result(r, rc);
        return NULL;
    }
    return adopt_result(r, __real__fdopen_r(r, fd, mode), 0);
}
int __wrap__fclose_r(struct _reent *r, FILE *file) {
    struct stream_node *node;
    if (find_stream(file, &node) < 0) {
        r->_errno = EBADF;
        return -1;
    }
    if (node && !authorized(r, node))
        return -1;
    return __real__fclose_r(r, file);
}
FILE *__wrap__freopen_r(struct _reent *r, const char *path, const char *mode, FILE *file) {
    struct stream_node *node;
    if (find_stream(file, &node) < 0) {
        r->_errno = EBADF;
        return NULL;
    }
    if (node && !authorized(r, node))
        return NULL;
    return adopt_result(r, __real__freopen_r(r, path, mode, file), 1);
}
FILE *__wrap_fopen(const char *path, const char *mode) {
    return __wrap__fopen_r(__getreent(), path, mode);
}
FILE *__wrap_fdopen(int fd, const char *mode) {
    return __wrap__fdopen_r(__getreent(), fd, mode);
}
int __wrap_fclose(FILE *file) {
    return __wrap__fclose_r(__getreent(), file);
}
FILE *__wrap_freopen(const char *path, const char *mode, FILE *file) {
    return __wrap__freopen_r(__getreent(), path, mode, file);
}
