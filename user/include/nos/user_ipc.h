/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS User-Space / Service Client IPC & POSIX Bridge API
 */

#ifndef __NOS_USER_IPC_H__
#define __NOS_USER_IPC_H__

#include <uapi/nos/abi.h>
#include <sys/types.h>
#include <sys/stat.h>

/* Client IPC helpers */
int nos_user_ipc_call(nos_handle_t ep_cap,
                      uint16_t opcode,
                      uint64_t r0,
                      uint64_t r1,
                      uint64_t r2,
                      uint64_t r3,
                      const void *buf,
                      uint32_t len,
                      struct nos_ipc_msg *out_resp);

int nos_user_ipc_recv(nos_handle_t ep_cap,
                      struct nos_ipc_msg *msg,
                      uint32_t timeout_ticks);

int nos_user_ipc_reply_wait(nos_handle_t ep_cap,
                            const struct nos_ipc_msg *reply,
                            struct nos_ipc_msg *next_req,
                            uint32_t timeout_ticks);

nos_handle_t nos_user_endpoint_create(void);
nos_handle_t nos_user_notification_create(void);

/* POSIX file descriptor bridge (routes over VFS IPC endpoint or local VFS) */
void nos_posix_bind_vfs_endpoint(nos_handle_t vfs_ep_cap);
nos_handle_t nos_posix_get_vfs_endpoint(void);

int nos_posix_open(const char *path, int flags, mode_t mode);
ssize_t nos_posix_read(int fd, void *buf, size_t count);
ssize_t nos_posix_write(int fd, const void *buf, size_t count);
int nos_posix_close(int fd);
off_t nos_posix_lseek(int fd, off_t offset, int whence);
int nos_posix_fstat(int fd, struct stat *st);

#endif /* __NOS_USER_IPC_H__ */
