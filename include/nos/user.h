/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_USER_H
#define NOS_USER_H
#include "abi.h"
#ifdef NOS_NEWLIB
#define _POSIX_THREADS 1
#define _UNIX98_THREAD_MUTEX_ATTRIBUTES 1
#define _POSIX_CLOCK_SELECTION 1
#define _POSIX_TIMERS 1
#define _POSIX_MONOTONIC_CLOCK 1
#endif
struct nos_time_value { int64_t seconds; uint32_t nanoseconds; };
void nos_time_from_ticks(uint64_t ticks, struct nos_time_value *value);
int nos_clock_snapshot(struct nos_time_value *value);
int nos_sleep_ticks64(uint64_t ticks, uint64_t *remaining);
int nos_user_check(const void *pointer, size_t size, unsigned protection);
int nos_user_copy(void *destination, const void *source, size_t size);
intptr_t nos_read_nowait(int fd, void *buffer, size_t count);
intptr_t nos_write_nowait(int fd, const void *buffer, size_t count);
intptr_t nos_posix_read(int fd, void *buffer, size_t count);
intptr_t nos_posix_write(int fd, const void *buffer, size_t count);
void nos_process_exit(int status) __attribute__((noreturn));
void nos_libc_task_cleanup(void);
int nos_rmdir(const char *path);
int nos_rename(const char *old_path, const char *new_path);
int nos_readdir(int fd, size_t index, char *name, size_t size);
int nos_dup(int fd);
int nos_dup_min(int fd, unsigned minimum);
int nos_getflags(int fd);
int nos_setflags(int fd, unsigned flags);
int nos_dup2(int fd, int target);
int nos_pipe(int descriptors[2]);
int nos_pipe2(int descriptors[2], unsigned flags);
int nos_mount(const char *path, unsigned backend);
int nos_sync(void);
#endif
