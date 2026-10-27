/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * POSIX File Descriptor Client Bridge (IPC over NOS_VFS_OP_* + Local Fastpath)
 */

#include <nos/syscall_arch.h>
#include <nos/user_ipc.h>
#include <uapi/nos/errno.h>
#include <fs/fs.h>
#include <string.h>

static nos_handle_t g_vfs_ep_cap = NOS_HANDLE_INVALID;

void nos_posix_bind_vfs_endpoint(nos_handle_t vfs_ep_cap)
{
    g_vfs_ep_cap = vfs_ep_cap;
}

nos_handle_t nos_posix_get_vfs_endpoint(void)
{
    return g_vfs_ep_cap;
}

int nos_posix_open(const char *path, int flags, mode_t mode)
{
    struct nos_ipc_msg resp;
    int rc;

    if (path == NULL) {
        return -EINVAL;
    }

    if (g_vfs_ep_cap != NOS_HANDLE_INVALID) {
        rc = nos_user_ipc_call(g_vfs_ep_cap,
                               NOS_VFS_OP_OPEN,
                               (uint64_t)flags,
                               (uint64_t)mode,
                               0ULL,
                               0ULL,
                               path,
                               (uint32_t)(strlen(path) + 1U),
                               &resp);
        return (rc < 0) ? rc : (int)resp.regs[0];
    }

    return nos_open(path, flags, mode);
}

ssize_t nos_posix_read(int fd, void *buf, size_t count)
{
    struct nos_ipc_msg resp;
    int rc;

    if (buf == NULL && count > 0U) {
        return -EINVAL;
    }

    if (g_vfs_ep_cap != NOS_HANDLE_INVALID) {
        rc = nos_user_ipc_call(g_vfs_ep_cap,
                               NOS_VFS_OP_READ,
                               (uint64_t)fd,
                               (uint64_t)count,
                               0ULL,
                               0ULL,
                               buf,
                               (uint32_t)count,
                               &resp);
        return (rc < 0) ? (ssize_t)rc : (ssize_t)resp.regs[0];
    }

    return nos_read(fd, buf, count);
}

ssize_t nos_posix_write(int fd, const void *buf, size_t count)
{
    struct nos_ipc_msg resp;
    int rc;

    if (buf == NULL && count > 0U) {
        return -EINVAL;
    }

    if (g_vfs_ep_cap != NOS_HANDLE_INVALID) {
        rc = nos_user_ipc_call(g_vfs_ep_cap,
                               NOS_VFS_OP_WRITE,
                               (uint64_t)fd,
                               (uint64_t)count,
                               0ULL,
                               0ULL,
                               buf,
                               (uint32_t)count,
                               &resp);
        return (rc < 0) ? (ssize_t)rc : (ssize_t)resp.regs[0];
    }

    return nos_write(fd, buf, count);
}

int nos_posix_close(int fd)
{
    struct nos_ipc_msg resp;

    if (g_vfs_ep_cap != NOS_HANDLE_INVALID) {
        return nos_user_ipc_call(g_vfs_ep_cap,
                                 NOS_VFS_OP_CLOSE,
                                 (uint64_t)fd,
                                 0ULL,
                                 0ULL,
                                 0ULL,
                                 NULL,
                                 0U,
                                 &resp);
    }

    return nos_close(fd);
}

off_t nos_posix_lseek(int fd, off_t offset, int whence)
{
    struct nos_ipc_msg resp;
    int rc;

    if (g_vfs_ep_cap != NOS_HANDLE_INVALID) {
        rc = nos_user_ipc_call(g_vfs_ep_cap,
                               NOS_VFS_OP_LSEEK,
                               (uint64_t)fd,
                               (uint64_t)offset,
                               (uint64_t)whence,
                               0ULL,
                               NULL,
                               0U,
                               &resp);
        return (rc < 0) ? (off_t)rc : (off_t)resp.regs[0];
    }

    return (off_t)nos_lseek(fd, (loff_t)offset, whence);
}

int nos_posix_fstat(int fd, struct stat *st)
{
    struct nos_ipc_msg resp;
    int rc;

    if (st == NULL) {
        return -EINVAL;
    }

    if (g_vfs_ep_cap != NOS_HANDLE_INVALID) {
        rc = nos_user_ipc_call(g_vfs_ep_cap,
                               NOS_VFS_OP_FSTAT,
                               (uint64_t)fd,
                               0ULL,
                               0ULL,
                               0ULL,
                               st,
                               (uint32_t)sizeof(*st),
                               &resp);
        return rc;
    }

    memset(st, 0, sizeof(*st));
    st->st_mode = (fd >= 0 && fd < 3) ? (S_IFCHR | 0666) : (S_IFREG | 0644);
    return 0;
}
