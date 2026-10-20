/* Copyright (C) 2026 NOS contributors. Shared RAM file backing. */
#ifndef NOS_VFS_FILE_DATA_H
#define NOS_VFS_FILE_DATA_H
#ifdef NOS_LINUX_COMPAT
static uint64_t file_inodes[NODE_COUNT], next_file_inode = 0x8000u;
static struct { uintptr_t handle; char *address; } file_backing[NODE_COUNT];
static int node_backing(unsigned index)
{
    if (file_backing[index].handle) return 0;
    struct nos_shared_request request = { .length = FILE_BYTES };
    intptr_t handle = nos_syscall(NOS_SYS_SHARED, NOS_SHARED_CREATE, (uintptr_t)&request, 0);
    if (nos_pointer_is_error(handle)) return (int)handle;
    request = (struct nos_shared_request){ .handle = (uintptr_t)handle, .length = FILE_BYTES, .prot = 3, .flags = 1 };
    intptr_t mapped = nos_syscall(NOS_SYS_SHARED, NOS_SHARED_MAP, (uintptr_t)&request, 0);
    if (nos_pointer_is_error(mapped)) {
        nos_syscall(NOS_SYS_SHARED, NOS_SHARED_RELEASE, (uintptr_t)&request, 0); return (int)mapped;
    }
    u_memcpy((void *)(uintptr_t)mapped, nodes[index].bytes, nodes[index].size);
    file_backing[index].handle = (uintptr_t)handle; file_backing[index].address = (void *)(uintptr_t)mapped;
    return 0;
}
#endif
static uint64_t node_inode(unsigned index)
{
#ifdef NOS_LINUX_COMPAT
    if (file_inodes[index]) return file_inodes[index];
#endif
    return index + 1u;
}
static char *node_bytes(struct vnode *node)
{
#ifdef NOS_LINUX_COMPAT
    unsigned index = (unsigned)(node - nodes);
    if (file_backing[index].address) return file_backing[index].address;
#endif
    return node->bytes;
}
static size_t node_limit(const struct vnode *node)
{ return node->persistent ? INLINE_BYTES : FILE_BYTES; }
static int node_reserve(struct vnode *node, size_t length)
{
    if (length > node_limit(node)) return -27;
#ifdef NOS_LINUX_COMPAT
    if (!node->persistent && length > INLINE_BYTES) return node_backing((unsigned)(node - nodes));
#else
    (void)node;
#endif
    return 0;
}
static void clear_node(unsigned index)
{
#ifdef NOS_LINUX_COMPAT
    if (file_backing[index].handle) {
        struct nos_shared_request request = { .handle = file_backing[index].handle };
        nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)file_backing[index].address, FILE_BYTES, 0);
        nos_syscall(NOS_SYS_SHARED, NOS_SHARED_RELEASE, (uintptr_t)&request, 0);
        u_memset(&file_backing[index], 0, sizeof(file_backing[index]));
    }
#endif
#ifdef NOS_LINUX_COMPAT
    file_inodes[index] = 0; permission_clear(index);
#endif
    u_memset(&nodes[index], 0, sizeof(nodes[index]));
}
static int shared_file(struct nos_message *message, struct fd_table *table)
{
#ifndef NOS_LINUX_COMPAT
    (void)table; message->value = -NOS_ENOTSUP;
#else
    if (message->arg < 0 || (uintptr_t)message->arg >= FDS || !table->fd[message->arg]) { message->value = -NOS_EBADF; return 1; }
    struct description *description = &opened[table->fd[message->arg] - 1];
    if (description->kind || NODE_TYPE(&nodes[description->node]) != MODE_FILE || nodes[description->node].persistent) { message->value = -NOS_ENOTSUP; return 1; }
    unsigned protection = (unsigned)message->value;
    if (message->value < 0 || protection & ~3u || (description->flags & 3) == NOS_O_WRONLY ||
        ((protection & 2) && !(description->flags & 3))) { message->value = -NOS_EACCES; return 1; }
    int rc = node_backing(description->node); if (rc) { message->value = rc; return 1; }
    struct nos_shared_request request = { .handle = file_backing[description->node].handle, .target = (uintptr_t)message->sender, .prot = (description->flags & 3) == NOS_O_RDWR ? 3u : 1u };
    message->value = nos_syscall(NOS_SYS_SHARED, NOS_SHARED_GRANT, (uintptr_t)&request, 0);
    if (!nos_pointer_is_error(message->value)) { uint64_t bytes = nodes[description->node].size; u_memcpy(message->data, &bytes, 8); message->len = 8; }
#endif
    return 1;
}
#endif
