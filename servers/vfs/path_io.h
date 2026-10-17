/* Copyright (C) 2026 NOS contributors. Bounded paths stay in one IPC call. */
#ifndef NOS_VFS_PATH_IO_H
#define NOS_VFS_PATH_IO_H
struct path_payload { char bytes[PATH_DATA_MAX]; size_t len; };
static int path_operation(unsigned op)
{
    switch (op) {
    case NOS_VFS_OPEN: case NOS_VFS_MKDIR: case NOS_VFS_UNLINK:
    case NOS_VFS_RMDIR: case NOS_VFS_RENAME: case NOS_VFS_MOUNT:
    case NOS_VFS_OPENAT: case NOS_VFS_STATAT: case NOS_VFS_CHDIR:
    case NOS_VFS_READLINK: case NOS_VFS_CHMOD: case NOS_VFS_SYMLINK:
    case NOS_VFS_LINK: case NOS_VFS_MKDIRAT: case NOS_VFS_TRUNCATE:
    case NOS_VFS_UNLINKAT: case NOS_VFS_RENAMEAT:
#ifdef NOS_LINUX_COMPAT
    case NOS_VFS_RESOLVE_PATH:
#endif
        return 1;
    default: return 0;
    }
}
#ifdef NOS_LINUX_COMPAT
static int path_transfer(struct nos_message *message, void *buffer, size_t count, unsigned direction)
{
    struct nos_ipc_xfer request = { .sender = message->sender, .token = message->token,
        .buffer = (uintptr_t)buffer, .count = count, .direction = direction };
    return (int)nos_syscall(NOS_SYS_IPC_XFER, (uintptr_t)&request, 0, 0);
}
#endif
static int path_input(struct nos_message *message, struct path_payload *payload)
{
    payload->len = message->len;
    if (!path_operation(message->op)) return 0;
#ifdef NOS_LINUX_COMPAT
    if (message->bulk_in_len) {
        if (message->bulk_in_len > sizeof(payload->bytes)) return -NOS_ENAMETOOLONG;
        payload->len = message->bulk_in_len;
        return path_transfer(message, payload->bytes, payload->len, NOS_IPC_FETCH);
    }
#endif
    u_memcpy(payload->bytes, message->data, payload->len); return 0;
}
/* Strings require their complete terminating NUL; readlink may truncate. */
static void path_output(struct nos_message *message, const char *data, size_t count, int string)
{
#ifdef NOS_LINUX_COMPAT
    if (message->bulk_out_len) {
        if (count > message->bulk_out_len) {
            if (string) { message->value = -34; return; }
            count = message->bulk_out_len;
        }
        int rc = path_transfer(message, (void *)data, count, NOS_IPC_STORE);
        message->len = 0; message->value = rc ? rc : (intptr_t)count; return;
    }
#endif
    if (count > NOS_MSG_DATA) { message->value = -34; return; }
    u_memcpy(message->data, data, count); message->len = count; message->value = (intptr_t)count;
#ifndef NOS_LINUX_COMPAT
    (void)string;
#endif
}
#endif
