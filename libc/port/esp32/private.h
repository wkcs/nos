/* Copyright (C) 2026 NOS contributors. Pinned IDF/Newlib adapter internals. */
#ifndef NOS_ESP_LIBC_PRIVATE_H
#define NOS_ESP_LIBC_PRIVATE_H
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <errno.h>
#include <sys/reent.h>
#include "nos/user.h"

struct stream_node;
struct directory_node;
struct libc_state {
    unsigned pid;
    struct _reent *reent;
    struct stream_node *streams;
    struct directory_node *directories;
    unsigned stream_count, directory_count;
};
extern struct libc_state *nos_libc_states[NOS_TASKS];
int nos_libc_context(struct _reent *reent);
intptr_t nos_libc_result(struct _reent *reent, intptr_t result);
int nos_libc_pid(void);
int nos_idf_device_domain_owned(void);
intptr_t nos_libc_write(struct _reent *, int fd, const void *data, size_t size);
struct libc_state *nos_libc_state(void);
void nos_libc_directories_cleanup(struct libc_state *state, int orphan);
int nos_esp_libc_task_enter(void);
void nos_esp_libc_task_exit(void);
void nos_esp_libc_task_reclaim(unsigned pid, struct _reent *reent);
int nos_esp_libc_activate(void);
#endif
