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
#define NOS_DIRENT_RECORD_MAX ((19u + NOS_PATH_MAX + 7u) & ~7u)
void nos_time_from_ticks(uint64_t ticks, struct nos_time_value *value);
int nos_clock_snapshot(struct nos_time_value *value);
int nos_clock_read(unsigned clock, struct nos_time_value *value);
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
int nos_dup_cloexec(int fd, unsigned minimum);
int nos_getflags(int fd);
int nos_setflags(int fd, unsigned flags);
int nos_getfd(int fd);
int nos_setfd(int fd, unsigned flags);
int nos_dup2(int fd, int target);
int nos_dup3(int fd, int target, unsigned flags);
int nos_pipe(int descriptors[2]);
int nos_pipe2(int descriptors[2], unsigned flags);
int nos_mount(const char *path, unsigned backend);
int nos_sync(void);
#define NOS_AT_FDCWD (-100)
#define NOS_AT_SYMLINK_NOFOLLOW 0x100
#define NOS_AT_EMPTY_PATH 0x1000
int nos_openat(int directory, const char *path, unsigned flags);
int nos_openat_mode(int directory, const char *path, unsigned flags, unsigned mode);
int nos_mkdirat(int directory, const char *path, unsigned mode);
int nos_ftruncate(int fd, int64_t length);
int nos_truncate(const char *path, int64_t length);
int nos_umask(unsigned mask);
int nos_fchmod(int fd, unsigned mode);
int nos_chmod_at(int directory, const char *path, unsigned mode);
int nos_symlink_at(const char *target, int directory, const char *path);
int nos_link_at(int old_directory, const char *old_path, int new_directory, const char *new_path);
int nos_unlinkat(int directory, const char *path, unsigned flags);
int nos_renameat(int old_directory, const char *old_path, int new_directory, const char *new_path);
intptr_t nos_pread(int fd, void *buffer, size_t count, int64_t offset);
intptr_t nos_pwrite(int fd, const void *buffer, size_t count, int64_t offset);
int nos_stat64_fd(int fd, struct nos_stat64 *st);
int nos_stat_at(int directory, const char *path, unsigned flags, struct nos_stat64 *st);
intptr_t nos_getdents(int fd, void *buffer, size_t capacity);
intptr_t nos_getcwd(char *buffer, size_t capacity);
int nos_chdir(const char *path);
int nos_fchdir(int fd);
intptr_t nos_readlink_at(int directory, const char *path, char *buffer, size_t capacity);
intptr_t nos_resolve_at(int directory, const char *path, char *buffer, size_t capacity);
#endif
