/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
#include "poll_abi.h"
#include "../drivers/devices.h"
#include "../fatfs/adapter.h"
#ifndef NOS_PATH_MAX
#ifdef NOS_LINUX_COMPAT
#define NOS_PATH_MAX 4096u
#else
#define NOS_PATH_MAX 64u
#endif
#endif
#ifdef NOS_LINUX_COMPAT
#define PATH_DATA_MAX (2u * NOS_PATH_MAX)
#else
#define PATH_DATA_MAX NOS_MSG_DATA
#endif
#define JOINED_PATH_MAX (2u * NOS_PATH_MAX)
#ifdef NOS_LINUX_COMPAT
#define NODE_COUNT 32u
#else
#define NODE_COUNT 16u
#endif
#define DISK_NODE_COUNT 16u
#define INLINE_BYTES 512u
#ifdef NOS_LINUX_COMPAT
#define FILE_BYTES 8192u
#else
#define FILE_BYTES INLINE_BYTES
#endif
#define FDS NOS_FD_LIMIT
#define OPEN_COUNT (NOS_PROCESSES * FDS)
#ifdef NOS_LINUX_COMPAT
#ifndef NOS_IPC_PROCESS_NOTICE_LIMIT
#define NOS_IPC_PROCESS_NOTICE_LIMIT (4u * NOS_PROCESSES)
#endif
/* Live groups, queued parent references, delivered fork and transient copies. */
#define FD_TABLE_COUNT (NOS_PROCESSES + NOS_IPC_PROCESS_NOTICE_LIMIT + 3u)
#else
#define FD_TABLE_COUNT NOS_PROCESSES
#endif
#define PIPE_COUNT 2u
#ifdef NOS_LINUX_COMPAT
#define PIPE_BYTES NOS_PIPE_BUF
#else
#define PIPE_BYTES 256u
#endif
#define MODE_DIR 0040000u
#define MODE_FILE 0100000u
#define MODE_CHAR 0020000u
#define MODE_FIFO 0010000u
#define MODE_LINK 0120000u
#define NODE_TYPE(n) ((n)->mode & 0170000u)
#define NO_NODE 0xffffu
#ifndef NOS_O_DIRECTORY
#define NOS_O_DIRECTORY 0x10000u
#define NOS_O_NOFOLLOW 0x20000u
#define NOS_O_CLOEXEC 0x80000u
#define NOS_O_EXCL 0x80u
#endif
#include "path_storage.h"
struct vnode {
#ifdef NOS_LINUX_COMPAT
    char *name;
#else
    char name[NOS_PATH_MAX];
#endif
    char bytes[INLINE_BYTES]; size_t size; unsigned mode; uint16_t used, linked, refs, persistent;
};
struct description { uint64_t offset, size; uint32_t flags; uint16_t node, refs, kind, pipe; };
/* Every table may alias one OFD in every FD, with one extra pin per blocked
 * task. Process capacity bounds tables; task capacity bounds syscall pins. */
_Static_assert(FD_TABLE_COUNT * FDS + NOS_TASKS < UINT16_MAX, "OFD references fit in 16 bits");
_Static_assert(OPEN_COUNT <= UINT16_MAX, "OFD indices fit in FD entries");
_Static_assert(sizeof(struct description) == 32, "compact OFD layout");
#ifdef NOS_LINUX_COMPAT
struct directory_ref { uint16_t node, kind; uint32_t size; };
_Static_assert(sizeof(struct directory_ref) == 8, "compact pinned directory reference");
#endif
struct fd_table {
    intptr_t pid; unsigned short fd[FDS]; unsigned fd_flags; unsigned short mask;
#ifdef NOS_LINUX_COMPAT
    struct directory_ref cwd;
#else
    char cwd[NOS_PATH_MAX];
#endif
};
struct pipe_buffer { char bytes[PIPE_BYTES]; unsigned head, size, readers, writers; };
#ifdef NOS_LINUX_COMPAT
static void permission_clear(unsigned index);
#endif
static void record_lock_close(intptr_t owner, const struct description *description);
static struct vnode nodes[NODE_COUNT];
static struct description opened[OPEN_COUNT];
static struct fd_table fds[FD_TABLE_COUNT];
static struct pipe_buffer pipes[PIPE_COUNT];
#ifdef NOS_LINUX_COMPAT
/* Positional reads never change the OFD. Keep bounded transport staging off
 * the service stack and independent of future increases to the IPC ceiling. */
static char positioned_read_scratch[512];
#endif
static unsigned disk_mounted;
static unsigned fat_mounted;
static char fat_path[VFS_PERSIST_PATH_MAX];
static unsigned disk_generation;
static char mount_path[VFS_PERSIST_PATH_MAX];
#include "file_data.h"
static int archive_lookup(const char *, unsigned *, uint64_t *);
static int canonical_path(const char *, size_t, char [NOS_PATH_MAX]);
static int canonical_path_from(const char *, size_t, char [NOS_PATH_MAX], size_t);
static int prefix(const char *s, const char *p)
{ while (*p) if (*s++ != *p++) return 0; return 1; }
static int child_path(const char *s, const char *p)
{ size_t n = u_strlen(p); return prefix(s, p) && (n == 1 || s[n] == '/'); }
static int backend(const char *p)
{
#ifdef NOS_LINUX_COMPAT
    /* POSIX named IPC objects are RAM files under a writable tmpfs namespace. */
    if (u_streq(p, "/dev/shm") || child_path(p, "/dev/shm")) return 0;
#endif
    return child_path(p, "/dev") || u_streq(p, "/dev") ? 1 : child_path(p, "/proc") || u_streq(p, "/proc") ? 2 : 0; }
static int normalize(const char *src, size_t len, char *dst)
{
    if (!len || len > NOS_PATH_MAX || src[0] != '/' || src[len - 1]) return -NOS_EINVAL;
    size_t end = 1, i = 1; dst[0] = '/';
    while (i < len - 1) {
        while (i < len - 1 && src[i] == '/') i++;
        size_t start = i;
        while (i < len - 1 && src[i] != '/') { if (!src[i]) return -NOS_EINVAL; i++; }
        size_t n = i - start;
        if (!n || (n == 1 && src[start] == '.')) continue;
        if (n == 2 && src[start] == '.' && src[start + 1] == '.') {
            if (end > 1) { while (end > 1 && dst[end - 1] != '/') end--; if (end > 1) end--; }
            continue;
        }
        if (end > 1) dst[end++] = '/';
        if (end + n >= NOS_PATH_MAX) return -NOS_ENAMETOOLONG;
        u_memcpy(dst + end, src + start, n); end += n;
    }
    dst[end] = 0;
    /* A lexical traversal must not change the selected privileged mount. */
    if (backend(src) != backend(dst)) return -NOS_EACCES;
    if (fat_mounted && child_path(dst, fat_path))
        for (size_t j = u_strlen(fat_path); dst[j]; j++) if (dst[j] >= 'a' && dst[j] <= 'z') dst[j] -= 32;
    return 0;
}
static int path_equal(const char *a, const char *b, unsigned fold)
{
    while (*a && *b) { unsigned x = (unsigned char)*a++, y = (unsigned char)*b++; if (fold) { if (x >= 'a' && x <= 'z') x -= 32; if (y >= 'a' && y <= 'z') y -= 32; } if (x != y) return 0; }
    return !*a && !*b;
}
static const char *fat_relative(const char *path)
{ const char *p = path + u_strlen(fat_path); return *p ? p : "/"; }
static int lookup_entry(const char *path)
{
    for (unsigned i = 0; i < NODE_COUNT; i++)
        if (nodes[i].used && nodes[i].linked && path_equal(path, nodes[i].name, nodes[i].persistent == 2)) return (int)i;
    return -NOS_ENOENT;
}
static unsigned inode_of(unsigned entry)
{ return nodes[entry].used == 2 ? (unsigned)nodes[entry].size : entry; }
static int lookup(const char *path)
{ int entry = lookup_entry(path); return entry < 0 ? entry : (int)inode_of((unsigned)entry); }
static unsigned node_links(unsigned inode)
{
    unsigned count = nodes[inode].linked != 0;
    for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].used == 2 && nodes[i].linked && nodes[i].size == inode) count++;
    return count;
}
static void remove_entry(unsigned entry)
{
    if (nodes[entry].used == 2) { clear_node(entry); return; }
    struct vnode *node = &nodes[entry];
    for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].used == 2 && nodes[i].linked && nodes[i].size == entry) {
        u_memcpy(node->name, nodes[i].name, u_strlen(nodes[i].name) + 1);
        clear_node(i); return;
    }
    node->linked = 0; if (!node->refs) clear_node((unsigned)(node - nodes));
}
static int parent_of(const char *path)
{
    PATH_SCOPE;
    PATH_BUFFER(parent, NOS_PATH_MAX); if (PATH_MISSING(parent)) return -NOS_ENOMEM;
    size_t n = u_strlen(path);
    while (n > 1 && path[n - 1] != '/') n--;
    if (n > 1) n--;
    u_memcpy(parent, path, n); parent[n] = 0;
    int p = lookup(parent);
    return p < 0 ? p : NODE_TYPE(&nodes[p]) == MODE_DIR ? p : -NOS_ENOTDIR;
}
#include "permissions.h"
#ifdef NOS_LINUX_COMPAT
static void permission_clear(unsigned index) { u_memset(&security[index], 0, sizeof(security[index])); }
#endif
static int create_sized(const char *path, unsigned mode, size_t reservation)
{
    PATH_SCOPE;
    if (u_streq(path, "/")) return -NOS_EEXIST;
    int p = parent_of(path);
    if (p < 0) {
        PATH_BUFFER(parent, NOS_PATH_MAX); if (PATH_MISSING(parent)) return -NOS_ENOMEM;
        size_t end = u_strlen(path);
        while (end > 1 && path[end - 1] != '/') end--;
        if (end > 1) end--;
        u_memcpy(parent, path, end); parent[end] = 0;
        unsigned kind; uint64_t size;
        if (archive_lookup(parent, &kind, &size) >= 0) return -30;
        return p;
    }
    if (backend(path)) return -NOS_EACCES;
    int permission = permission_node((unsigned)p, 3); if (permission) return permission;
#ifdef NOS_LINUX_COMPAT
    if (nodes[p].persistent && subject.credentials.euid) return -NOS_ENOTSUP;
    if ((mode & 0170000u) != MODE_DIR && (mode & 06000u)) return -NOS_ENOTSUP;
    int64_t now; permission = permission_clock(&now); if (permission) return permission;
#endif
    if (nodes[p].persistent && u_strlen(path) >= 64) return -NOS_ENAMETOOLONG;
    for (unsigned i = 0; i < NODE_COUNT; i++) if (!nodes[i].used) {
        #ifdef NOS_LINUX_COMPAT
        if (next_file_inode == UINT64_MAX) return -NOS_ENOSPC;
#endif
        clear_node(i);
        int reserved = node_reserve(&nodes[i], reservation); if (reserved) { clear_node(i); return reserved; }
        if (nodes[p].persistent == 2) { int rc = (mode & 0170000u) == MODE_DIR ? nos_fat_mkdir(fat_relative(path)) : nos_fat_write(fat_relative(path), "", 0); if (rc) { clear_node(i); return rc; } }
        u_memcpy(nodes[i].name, path, u_strlen(path) + 1);
        nodes[i].mode = mode; nodes[i].used = nodes[i].linked = 1;
        nodes[i].persistent = nodes[p].persistent;
#ifdef NOS_LINUX_COMPAT
        file_inodes[i] = next_file_inode++;
        security[i].uid = nodes[p].persistent ? 0 : subject.credentials.euid;
        security[i].gid = nodes[p].persistent ? 0 : (nodes[p].mode & 02000u) ? security[p].gid : subject.credentials.egid;
        if (NODE_TYPE(&nodes[i]) == MODE_DIR && (nodes[p].mode & 02000u)) nodes[i].mode |= 02000u;
        permission_touch(i, 7, now); permission_touch((unsigned)p, 6, now);
#endif
        return (int)i;
    }
    return -NOS_ENOSPC;
}
static int create(const char *path, unsigned mode)
{ return create_sized(path, mode, 0); }
static void drop_description(unsigned d)
{
    if (d >= OPEN_COUNT || !opened[d].refs) return;
    if (!--opened[d].refs && d >= 3) {
        struct description *o = &opened[d];
        if (o->kind == 2) { struct pipe_buffer *p = &pipes[o->pipe]; if (o->flags & 3) p->writers--; else p->readers--; }
        else if (o->kind < 3 && o->node < NODE_COUNT) {
            struct vnode *n = &nodes[o->node]; if (n->refs) n->refs--;
            if (!n->linked && !n->refs) clear_node((unsigned)(n - nodes));
        }
        u_memset(o, 0, sizeof(*o));
    }
}
static void close_descriptor(struct fd_table *table, unsigned fd)
{
    if (fd >= FDS || !table->fd[fd]) return;
    unsigned description = table->fd[fd] - 1;
    record_lock_close(table->pid, &opened[description]);
    table->fd[fd] = 0; table->fd_flags &= ~(1u << fd); drop_description(description);
}
#ifdef NOS_LINUX_COMPAT
static void directory_ref_drop(struct directory_ref *ref)
{
    if (!ref->kind && ref->node < NODE_COUNT) {
        struct vnode *node = &nodes[ref->node];
        if (node->refs) node->refs--;
        if (!node->refs && !node->linked) clear_node(ref->node);
    }
    *ref = (struct directory_ref){ .node = NO_NODE };
}
static int directory_ref_hold(const struct directory_ref *ref)
{
    if (ref->kind) return ref->kind == 4u ? 0 : -NOS_ENOTDIR;
    if (ref->node >= NODE_COUNT || !nodes[ref->node].used || NODE_TYPE(&nodes[ref->node]) != MODE_DIR) return -NOS_ENOTDIR;
    if (nodes[ref->node].refs == UINT16_MAX) return -NOS_EOVERFLOW;
    nodes[ref->node].refs++; return 0;
}
static int directory_ref_set(struct fd_table *table, struct directory_ref ref)
{
    int rc = directory_ref_hold(&ref); if (rc) return rc;
    directory_ref_drop(&table->cwd); table->cwd = ref; return 0;
}
#endif
static void release_table(struct fd_table *t)
{
    for (unsigned i = 0; i < FDS; i++) close_descriptor(t, i);
#ifdef NOS_LINUX_COMPAT
    if (t->pid >= 0) directory_ref_drop(&t->cwd);
#endif
    t->pid = -1;
}
static struct fd_table *table_for(intptr_t pid)
{
    struct fd_table *t = 0;
    for (unsigned i = 0; i < FD_TABLE_COUNT; i++) if (fds[i].pid == pid) return &fds[i];
    for (unsigned i = 0; i < FD_TABLE_COUNT; i++) if (fds[i].pid < 0) { t = &fds[i]; break; }
    if (!t) return 0;
    {
        release_table(t); t->pid = pid; t->fd_flags = 0; t->mask = 0022;
#ifdef NOS_LINUX_COMPAT
        t->cwd = (struct directory_ref){ .node = NO_NODE };
        if (directory_ref_set(t, (struct directory_ref){ .node = 0 })) { t->pid = -1; return NULL; }
#else
        t->cwd[0] = '/'; t->cwd[1] = 0;
#endif
        for (unsigned i = 0; i < 3; i++) { t->fd[i] = i + 1; opened[i].refs++; }
    }
    return t;
}
static int free_fd(struct fd_table *t)
{ for (unsigned i = 0; i < descriptor_limit; i++) if (!t->fd[i]) return (int)i; return -NOS_EMFILE; }
static int free_description(void)
{ for (unsigned i = 3; i < OPEN_COUNT; i++) if (!opened[i].refs) return (int)i; return -NOS_EMFILE; }
#include "archive.h"
static intptr_t read_data(struct description *d, char *data, size_t count)
{
    if ((d->flags & 3) == NOS_O_WRONLY) return -NOS_EBADF;
    if (!count) return 0;
    if (d->kind == ARCHIVE_DIRECTORY) return -NOS_EISDIR;
    if (d->kind == ARCHIVE_FILE) { intptr_t n = archive_read(d->node, d->offset, data, count); if (n > 0) d->offset += (uint64_t)n; return n; }
    if (d->kind == 1) {
        size_t n = 0;
        while (n < count) { intptr_t c = nos_device_uart_read(); if (c < 0) return n ? (intptr_t)n : c; data[n++] = (char)c; }
        return (intptr_t)n;
    }
    if (d->kind == 2) {
        struct pipe_buffer *p = &pipes[d->pipe];
        if (!p->size) return p->writers ? -NOS_EAGAIN : 0;
        if (count > p->size) count = p->size;
        for (size_t i = 0; i < count; i++) { data[i] = p->bytes[p->head]; p->head = (p->head + 1) % PIPE_BYTES; }
        p->size -= count; return (intptr_t)count;
    }
    struct vnode *n = &nodes[d->node];
    if (NODE_TYPE(n) == MODE_DIR) return -NOS_EISDIR;
    if (d->offset >= n->size) return 0;
    if (count > n->size - d->offset) count = n->size - d->offset;
    u_memcpy(data, node_bytes(n) + (size_t)d->offset, count); d->offset += count;
#ifdef NOS_LINUX_COMPAT
    int64_t now; if (!permission_clock(&now)) permission_touch(d->node, 1, now);
#endif
    return (intptr_t)count;
}
static intptr_t write_data(struct description *d, const char *data, size_t count)
{
    if (!(d->flags & 3)) return -NOS_EBADF;
    if (d->kind >= ARCHIVE_FILE) return -30;
    if (!count) return 0;
    if (d->kind == 1) {
        size_t done = 0;
        while (done < count) {
            size_t chunk = count - done; if (chunk > 1024) chunk = 1024;
            intptr_t written = nos_device_uart_write(data + done, chunk);
            if (written < 0) return done ? (intptr_t)done : written;
            if ((size_t)written > chunk) return done ? (intptr_t)done : -NOS_EIO;
            done += (size_t)written;
            if ((size_t)written < chunk) break;
        }
        return (intptr_t)done;
    }
    if (d->kind == 2) {
        struct pipe_buffer *p = &pipes[d->pipe];
        if (!p->readers) return -32; /* EPIPE, no unsupported signal emulation. */
        if (count > PIPE_BYTES - p->size) return -NOS_EAGAIN;
        for (size_t i = 0; i < count; i++) p->bytes[(p->head + p->size + i) % PIPE_BYTES] = data[i];
        p->size += count; return (intptr_t)count;
    }
    struct vnode *n = &nodes[d->node];
    if (NODE_TYPE(n) == MODE_DIR) return -NOS_EISDIR;
    if (backend(n->name) == 2) return -NOS_EACCES;
    if (d->flags & 0x400u) d->offset = (unsigned)n->size;
    size_t limit = node_limit(n);
    if (d->offset >= limit) return -NOS_ENOSPC;
    if (count > limit - d->offset) count = limit - d->offset;
#ifdef NOS_LINUX_COMPAT
    int64_t now; int clock = permission_clock(&now); if (clock) return clock;
#endif
    int reserve = node_reserve(n, (size_t)d->offset + count); if (reserve) return reserve;
    if (d->offset > n->size) u_memset(node_bytes(n) + n->size, 0, d->offset - n->size);
    u_memcpy(node_bytes(n) + d->offset, data, count); d->offset += count;
    if (d->offset > n->size) n->size = d->offset;
    if (n->persistent == 2 && n->linked) { int rc = nos_fat_write(fat_relative(n->name), node_bytes(n), n->size); if (rc) return rc; }
#ifdef NOS_LINUX_COMPAT
    permission_touch(d->node, 6, now); if (subject.credentials.euid) n->mode &= ~06000u;
#endif
    return (intptr_t)count;
}
#include "pipe_wait.h"
#include "record_lock.h"
static unsigned crc32(unsigned crc, const void *buffer, size_t count)
{
    const unsigned char *p = buffer;
    while (count--) { crc ^= *p++; for (unsigned j = 0; j < 8; j++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u))); }
    return crc;
}
#define BANK_SECTORS (1u + 2u * DISK_NODE_COUNT)
struct disk_header { uint32_t magic, version, generation, count, crc, header_crc; };
struct disk_node { uint32_t mode, size; char name[64]; };
static int disk_bank_check(unsigned bank, struct disk_header *header)
{
    unsigned char sector[512];
    int rc = nos_block_read(bank * BANK_SECTORS, sector); if (rc) return rc;
    u_memcpy(header, sector, sizeof(*header));
    if (header->magic != 0x4e4f5346u || header->version != 2 || header->count > DISK_NODE_COUNT || header->header_crc != ~crc32(~0u, header, offsetof(struct disk_header, header_crc))) return -NOS_EINVAL;
    unsigned crc = ~0u;
    for (unsigned i = 0; i < DISK_NODE_COUNT * 2; i++) { rc = nos_block_read(bank * BANK_SECTORS + 1 + i, sector); if (rc) return rc; crc = crc32(crc, sector, 512); }
    return (~crc == header->crc) ? 0 : -NOS_EIO;
}
static int disk_sync(void)
{
    if (!disk_mounted) return -NOS_ENOTSUP;
    if (disk_generation == UINT32_MAX) return -NOS_EOVERFLOW;
    for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].used && nodes[i].linked && nodes[i].persistent &&
        (u_strlen(nodes[i].name) >= 64 || nodes[i].size > INLINE_BYTES)) return -NOS_ENAMETOOLONG;
#ifdef NOS_LINUX_COMPAT
    unsigned persistent_count = 0;
    for (unsigned i = 0; i < NODE_COUNT; i++) persistent_count += nodes[i].used && nodes[i].linked && nodes[i].persistent;
    if (persistent_count > DISK_NODE_COUNT) return -NOS_ENOSPC;
    unsigned source = 0;
#endif
    unsigned bank = (disk_generation + 1) & 1u, crc = ~0u, count = 0;
    unsigned char sector[512];
    for (unsigned i = 0; i < DISK_NODE_COUNT; i++) {
#ifdef NOS_LINUX_COMPAT
        while (source < NODE_COUNT && !(nodes[source].used && nodes[source].linked && nodes[source].persistent)) source++;
        struct vnode empty = { 0 }, *n = source < NODE_COUNT ? &nodes[source++] : &empty;
#else
        struct vnode *n = &nodes[i];
#endif
        u_memset(sector, 0, sizeof(sector));
        if (n->used && n->linked && n->persistent) {
            struct disk_node entry = { .mode = n->mode, .size = (uint32_t)n->size };
            u_memcpy(entry.name, n->name, u_strlen(n->name) + 1); u_memcpy(sector, &entry, sizeof(entry)); count++;
        }
        crc = crc32(crc, sector, 512);
        int rc = nos_block_write(bank * BANK_SECTORS + 1 + 2 * i, sector); if (rc) return rc;
        u_memset(sector, 0, sizeof(sector));
        if (n->used && n->linked && n->persistent) u_memcpy(sector, n->bytes, n->size);
        crc = crc32(crc, sector, 512); rc = nos_block_write(bank * BANK_SECTORS + 2 + 2 * i, sector); if (rc) return rc;
    }
    int rc = nos_block_flush(); if (rc) return rc;
    struct disk_header h = { 0x4e4f5346u, 2, disk_generation + 1, count, ~crc, 0 };
    h.header_crc = ~crc32(~0u, &h, offsetof(struct disk_header, header_crc));
    u_memset(sector, 0, sizeof(sector)); u_memcpy(sector, &h, sizeof(h));
    rc = nos_block_write(bank * BANK_SECTORS, sector); if (rc) return rc;
    rc = nos_block_flush(); if (!rc) disk_generation++; return rc;
}
static int disk_mount(const char *path)
{
    if (u_strlen(path) >= 64) return -NOS_ENAMETOOLONG;
    if (disk_mounted || fat_mounted) return -NOS_EBUSY;
    if (backend(path) || u_streq(path, "/")) return -NOS_EACCES;
    int root = lookup(path);
    if (root >= 0 && NODE_TYPE(&nodes[root]) != MODE_DIR) return -NOS_ENOTDIR;
    for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].used && nodes[i].linked && child_path(nodes[i].name, path)) return -NOS_ENOTEMPTY;
    int rc = nos_block_init(); if (rc) return rc;
    struct disk_header headers[2]; int valid[2];
    for (unsigned i = 0; i < 2; i++) valid[i] = !disk_bank_check(i, &headers[i]);
    unsigned bank = valid[1] && (!valid[0] || headers[1].generation > headers[0].generation) ? 1 : 0;
    struct disk_node entries[DISK_NODE_COUNT]; unsigned char sector[512]; unsigned count = 0, roots = 0, free_nodes = 0;
    if (!valid[0] && !valid[1]) {
        /* An unformatted disk is zero-filled; corruption must never turn a
         * populated disk into a silently empty, writable filesystem. */
        for (unsigned i = 0; i < 2 * BANK_SECTORS; i++) {
            rc = nos_block_read(i, sector); if (rc) return rc;
            for (unsigned j = 0; j < sizeof(sector); j++) if (sector[j]) return -NOS_EIO;
        }
    }
    u_memset(entries, 0, sizeof(entries));
    for (unsigned i = 0; i < NODE_COUNT; i++) if (!nodes[i].used) free_nodes++;
    if (valid[bank]) {
        for (unsigned i = 0; i < DISK_NODE_COUNT; i++) {
            rc = nos_block_read(bank * BANK_SECTORS + 1 + 2 * i, sector); if (rc) return rc;
            u_memcpy(&entries[i], sector, sizeof(entries[i])); struct disk_node *entry = &entries[i];
            if (!entry->mode) continue;
            if (entry->size > INLINE_BYTES || entry->name[63] || !((entry->mode & 0170000u) == MODE_FILE || NODE_TYPE(entry) == MODE_DIR) || (NODE_TYPE(entry) == MODE_DIR && entry->size)) return -NOS_EINVAL;
            char canonical[VFS_PERSIST_PATH_MAX]; rc = normalize(entry->name, u_strlen(entry->name) + 1, canonical);
            if (rc || !u_streq(canonical, entry->name) || !(u_streq(entry->name, path) || child_path(entry->name, path))) return -NOS_EACCES;
            if (u_streq(entry->name, path)) { if ((entry->mode & 0170000u) != MODE_DIR) return -NOS_ENOTDIR; roots++; }
            for (unsigned j = 0; j < i; j++) if (entries[j].mode && u_streq(entries[j].name, entry->name)) return -NOS_EINVAL;
            count++;
        }
        if (count != headers[bank].count || roots != 1 || count > free_nodes + (root >= 0)) return -NOS_ENOSPC;
    }
    unsigned created = root < 0, installed = 0;
    if (root < 0) root = create(path, MODE_DIR);
    if (root < 0) return root;
    nodes[root].persistent = 1;
    if (valid[bank]) {
        for (unsigned i = 0; i < DISK_NODE_COUNT; i++) {
            struct disk_node *entry = &entries[i]; if (!entry->mode || u_streq(entry->name, path)) continue;
            int slot = -1; for (unsigned j = 0; j < NODE_COUNT; j++) if (!nodes[j].used) { slot = (int)j; break; }
            if (slot < 0) { rc = -NOS_ENOSPC; goto rollback; }
            rc = nos_block_read(bank * BANK_SECTORS + 2 + 2 * i, sector); if (rc) goto rollback;
            struct vnode *n = &nodes[slot]; clear_node((unsigned)(n - nodes));
            u_memcpy(n->name, entry->name, 64); n->size = entry->size; n->mode = entry->mode; n->used = n->linked = n->persistent = 1;
            u_memcpy(n->bytes, sector, n->size); installed |= 1u << slot;
        }
        for (unsigned i = 0; i < NODE_COUNT; i++) if ((installed & (1u << i)) && parent_of(nodes[i].name) < 0) { rc = -NOS_EINVAL; goto rollback; }
        disk_generation = headers[bank].generation;
    }
    u_memcpy(mount_path, path, u_strlen(path) + 1); disk_mounted = 1; return 0;
rollback:
    for (unsigned i = 0; i < NODE_COUNT; i++) if (installed & (1u << i)) clear_node(i);
    nodes[root].persistent = 0; if (created) clear_node((unsigned)root); return rc;
}
static int fat_import(const char *path, unsigned depth)
{
    if (depth > 8) return -NOS_EINVAL;
    for (unsigned index = 0;; index++) {
        char name[VFS_PERSIST_PATH_MAX], full[VFS_PERSIST_PATH_MAX]; unsigned mode; size_t size;
        int rc = nos_fat_list(fat_relative(path), index, name, &mode, &size);
        if (rc <= 0) return rc;
        if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) continue;
        size_t n = u_strlen(path), extra = u_strlen(name);
        if (n + 1 + extra >= sizeof(full) || size > INLINE_BYTES) return -NOS_ENOTSUP;
        u_memcpy(full, path, n); full[n++] = '/'; u_memcpy(full + n, name, extra + 1);
        int slot = -1; for (unsigned i = 0; i < NODE_COUNT; i++) if (!nodes[i].used) { slot = (int)i; break; }
        if (slot < 0) return -NOS_ENOSPC;
        struct vnode *node = &nodes[slot]; clear_node((unsigned)(node - nodes));
        u_memcpy(node->name, full, n + extra + 1); node->mode = mode | ((mode & 0170000u) == MODE_DIR ? 0777 : 0666); node->size = size; node->used = node->linked = 1; node->persistent = 2;
        if ((mode & 0170000u) == MODE_DIR) rc = fat_import(full, depth + 1);
        else { rc = nos_fat_read(fat_relative(full), node->bytes, size); if (rc >= 0) rc = (size_t)rc == size ? 0 : -NOS_EIO; }
        if (rc) return rc;
    }
}
static int fat_mount(const char *path, int format)
{
    if (u_strlen(path) >= 64) return -NOS_ENAMETOOLONG;
    if (disk_mounted || fat_mounted) return -NOS_EBUSY;
    if (backend(path) || u_streq(path, "/")) return -NOS_EACCES;
    int root = lookup(path);
    if (root >= 0 && NODE_TYPE(&nodes[root]) != MODE_DIR) return -NOS_ENOTDIR;
    for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].used && nodes[i].linked && child_path(nodes[i].name, path)) return -NOS_ENOTEMPTY;
    int rc = nos_fat_mount(format); if (rc) return rc;
    unsigned created = root < 0; if (root < 0) root = create(path, MODE_DIR); if (root < 0) return root;
    u_memcpy(fat_path, path, u_strlen(path) + 1); nodes[root].persistent = 2;
    rc = fat_import(path, 0);
    if (rc) {
        for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].persistent == 2 && (int)i != root) clear_node(i);
        nodes[root].persistent = 0; if (created) clear_node((unsigned)root); fat_path[0] = 0; return rc;
    }
    fat_mounted = 1; return 0;
}
#include "path_io.h"
static int path_stat(const char *path, struct nos_stat64 *st);
#include "metadata.h"
static int path_stat(const char *path, struct nos_stat64 *st)
{
    int node = lookup(path);
    struct description description = { 0 };
    if (node >= 0) description.node = (unsigned)node;
    else { unsigned kind; uint64_t size; int index = archive_lookup(path, &kind, &size); if (index < 0) return index;
        description.node = (unsigned)index; description.kind = kind; description.size = size; }
    return full_stat(&description, st);
}
static void service(struct nos_message *m)
{
    PATH_SCOPE;
    unsigned create_mode = 0666;
    if (m->op == NOS_VFS_OPENAT_MODE) {
        create_mode = (uint32_t)m->arg >> 20; m->arg = (uint32_t)m->arg & 0xfffffu; m->op = NOS_VFS_OPENAT;
    }
    if (m->sender < 0) { m->value = -NOS_EPERM; return; }
    if (m->len > NOS_MSG_DATA) { m->value = -NOS_EINVAL; return; }
#ifdef NOS_LINUX_COMPAT
    int authenticated = permission_subject(m); if (authenticated) { m->value = authenticated; return; }
#else
    if (m->op >= NOS_VFS_ACCESS && m->op <= NOS_VFS_FCHOWN) { m->value = -NOS_ENOTSUP; return; }
#endif
    collect_process_owners();
    struct fd_table *t = table_for((intptr_t)m->process);
    if (!t) { m->value = -NOS_ENOSPC; return; }
    if (m->op == NOS_VFS_RECORD_LOCK) { record_lock_service(m, t); return; }
    if (m->op == NOS_VFS_SHARED_FILE) { shared_file(m, t); return; }
#ifdef NOS_LINUX_COMPAT
    PATH_BUFFER(payload_bytes, PATH_DATA_MAX);
    if (PATH_MISSING(payload_bytes)) { m->value = -NOS_ENOMEM; return; }
    struct path_payload payload = { .bytes = payload_bytes };
#else
    struct path_payload payload;
#endif
    int input = path_input(m, &payload); if (input) { m->value = input; return; }
#ifdef NOS_LINUX_COMPAT
    if (credential_service(m, t, &payload)) return;
#endif
    if (metadata_service(m, t, &payload)) return;
    if (m->op == 32) {
        struct nos_pollfd records[8];
        if (m->len % sizeof(records[0]) || m->len > sizeof(records)) { m->value = -NOS_EINVAL; return; }
        unsigned count = (unsigned)(m->len / sizeof(records[0])), ready = 0;
        u_memcpy(records, m->data, m->len);
        for (unsigned i = 0; i < count; i++) {
            struct nos_pollfd *record = &records[i]; unsigned available = 0; record->revents = 0;
            if (record->fd < 0) continue;
            if ((unsigned)record->fd >= FDS || !t->fd[record->fd]) available = 32;
            else {
                struct description *description = &opened[t->fd[record->fd] - 1];
                if (description->kind == 2) {
                    struct pipe_buffer *pipe = &pipes[description->pipe];
                    if (description->flags & 3) {
                        if (pipe->size < PIPE_BYTES) available |= 4;
                        if (!pipe->readers) available |= 8;
                    } else {
                        if (pipe->size) available |= 1;
                        if (!pipe->writers) available |= 16;
                    }
                } else if (description->kind == 1) {
                    if ((description->flags & 3) != NOS_O_WRONLY && nos_uart_readable()) available |= 1;
                    if ((description->flags & 3) && nos_uart_writable()) available |= 4;
                } else available = 1 | 4;
            }
            if (available & NOS_POLLIN) available |= NOS_POLLRDNORM;
            if (available & NOS_POLLOUT) available |= NOS_POLLWRNORM;
            record->revents = (int16_t)(available & ((unsigned)(uint16_t)record->events | 8u | 16u | 32u));
            ready += record->revents != 0;
        }
        u_memcpy(m->data, records, m->len); m->value = ready; return;
    }
    if (m->op == NOS_VFS_GETRANDOM) {
        if (m->arg < 0 || (uintptr_t)m->arg > 3u) { m->value = -NOS_EINVAL; return; }
        m->value = nos_random_read(m->data, m->len, (unsigned)m->arg);
        if (m->value >= 0) m->len = (size_t)m->value;
        return;
    }
#ifdef NOS_LINUX_COMPAT
    if (m->op == NOS_VFS_RESOLVE_PATH) {
        PATH_BUFFER(path, NOS_PATH_MAX); if (PATH_MISSING(path)) { m->value = -NOS_ENOMEM; return; }
        struct nos_stat64 st;
        uintptr_t flags = (uintptr_t)m->value;
        if (flags & ~((uintptr_t)NOS_RESOLVE_EXEC)) { m->value = -NOS_EINVAL; return; }
        int rc = metadata_path(t, m->arg, payload.bytes, payload.len, path, 1);
        if (!rc) rc = path_stat(path, &st);
        if (!rc && (flags & NOS_RESOLVE_EXEC) && (st.mode & 0170000u) != MODE_FILE) rc = -NOS_EACCES;
        if (!rc && (flags & NOS_RESOLVE_EXEC)) rc = permission_path(path, 1);
        if (rc) { m->value = rc; return; }
        path_output(m, path, u_strlen(path) + 1, 1); return;
    }
#endif
    if (m->op == NOS_VFS_GETCWD) {
#ifdef NOS_LINUX_COMPAT
        PATH_BUFFER(path, NOS_PATH_MAX); if (PATH_MISSING(path)) { m->value = -NOS_ENOMEM; return; }
        int rc = cwd_path(t, path); if (rc) { m->value = rc; return; }
        path_output(m, path, u_strlen(path) + 1, 1);
#else
        path_output(m, t->cwd, u_strlen(t->cwd) + 1, 1);
#endif
        return;
    }
    if (m->op == NOS_VFS_OPENAT) {
        PATH_BUFFER(path, NOS_PATH_MAX); if (PATH_MISSING(path)) { m->value = -NOS_ENOMEM; return; }
        int rc = resolve_path(t, m->value, payload.bytes, payload.len, path);
        if (rc) { m->value = rc; return; }
        payload.len = u_strlen(path) + 1; u_memcpy(payload.bytes, path, payload.len); payload.resolved = 1; m->op = NOS_VFS_OPEN;
    }
    if (m->op == NOS_VFS_STATAT || m->op == NOS_VFS_CHDIR || m->op == NOS_VFS_READLINK) {
        PATH_BUFFER(path, NOS_PATH_MAX); if (PATH_MISSING(path)) { m->value = -NOS_ENOMEM; return; }
        struct nos_stat64 st;
        if (m->op == NOS_VFS_STATAT && (m->value < 0 || ((uintptr_t)m->value & ~(0x100u | 0x1000u)))) { m->value = -NOS_EINVAL; return; }
        if (m->op == NOS_VFS_STATAT && (m->value & 0x1000) && payload.len == 1 && !payload.bytes[0] && m->arg != AT_CURRENT_DIRECTORY) {
            if (m->arg < 0 || (uintptr_t)m->arg >= FDS || !t->fd[m->arg]) { m->value = -NOS_EBADF; return; }
            m->value = full_stat(&opened[t->fd[m->arg] - 1], &st);
        } else {
            int rc = resolve_path(t, m->op == NOS_VFS_CHDIR ? AT_CURRENT_DIRECTORY : m->arg, payload.bytes, payload.len, path);
            if (rc) { m->value = rc; return; }
            rc = follow_links(path, m->op != NOS_VFS_READLINK && !(m->op == NOS_VFS_STATAT && (m->value & 0x100)));
            if (rc) { m->value = rc; return; }
            if (m->op == NOS_VFS_READLINK) {
                int node = lookup(path);
                if (node < 0) m->value = node;
                else if (NODE_TYPE(&nodes[node]) != MODE_LINK) m->value = -NOS_EINVAL;
                else path_output(m, node_bytes(&nodes[node]), nodes[node].size, 0);
                return;
            }
            m->value = path_stat(path, &st);
        }
        if (m->value < 0) return;
        if (m->op == NOS_VFS_CHDIR) {
            if ((st.mode & 0170000) != MODE_DIR) { m->value = -NOS_ENOTDIR; return; }
            int search = permission_path(path, 1); if (search) { m->value = search; return; }
#ifdef NOS_LINUX_COMPAT
            m->value = cwd_set_path(t, path);
#else
            u_memcpy(t->cwd, path, u_strlen(path) + 1); m->value = 0;
#endif
        } else if (m->op == NOS_VFS_READLINK) m->value = -NOS_EINVAL;
        else { u_memcpy(m->data, &st, sizeof(st)); m->len = sizeof(st); m->value = 0; }
        return;
    }
    if (m->op == NOS_VFS_DEVICE_STATS) {
        if (m->arg == 1) { struct nos_uart_stats stats; nos_uart_get_stats(&stats); u_memcpy(m->data, &stats, sizeof(stats)); m->len = sizeof(stats); m->value = 0; return; }
        if (m->arg) { m->value = -NOS_EINVAL; return; }
        struct nos_block_stats stats = { nos_block_interrupts(), nos_block_requests(), nos_block_poll_fallbacks(), nos_block_capacity(), nos_block_version(), 0 };
        u_memcpy(m->data, &stats, sizeof(stats)); m->len = sizeof(stats); m->value = 0; return;
    }
    if (m->op == NOS_VFS_SYNC) { m->value = fat_mounted ? nos_fat_sync() : disk_sync(); return; }
    if (m->op == NOS_VFS_PIPE) {
        if (m->arg < 0 || ((uintptr_t)m->arg & ~(NOS_O_NONBLOCK | NOS_O_CLOEXEC))) { m->value = -NOS_EINVAL; return; }
        unsigned flags = (unsigned)m->arg;
#ifdef NOS_LINUX_COMPAT
        int64_t now; int clock = permission_clock(&now); if (clock) { m->value = clock; return; }
#endif
        int fd0 = free_fd(t), fd1 = -1, d0 = free_description(), d1 = -1, pipe = -1;
        if (fd0 >= 0) for (unsigned i = fd0 + 1; i < descriptor_limit; i++) if (!t->fd[i]) { fd1 = (int)i; break; }
        if (d0 >= 0) for (unsigned i = d0 + 1; i < OPEN_COUNT; i++) if (!opened[i].refs) { d1 = (int)i; break; }
        for (unsigned i = 0; i < PIPE_COUNT; i++) if (!pipes[i].readers && !pipes[i].writers) { pipe = (int)i; break; }
        if (fd0 < 0 || fd1 < 0 || d0 < 0 || d1 < 0) { m->value = -NOS_EMFILE; return; }
        if (pipe < 0) { m->value = -NOS_ENOSPC; return; }
#ifdef NOS_LINUX_COMPAT
        pipe_security[pipe] = (struct inode_security){ .uid = subject.credentials.euid, .gid = subject.credentials.egid,
            .atime = now, .mtime = now, .ctime = now };
#endif
        u_memset(&pipes[pipe], 0, sizeof(pipes[pipe])); pipes[pipe].readers = pipes[pipe].writers = 1;
        opened[d0] = (struct description){ .refs = 1, .kind = 2, .pipe = pipe, .flags = flags };
        opened[d1] = (struct description){ .refs = 1, .kind = 2, .pipe = pipe, .flags = NOS_O_WRONLY | flags };
        t->fd[fd0] = d0 + 1; t->fd[fd1] = d1 + 1;
        t->fd_flags &= ~((1u << fd0) | (1u << fd1));
        if (flags & NOS_O_CLOEXEC) t->fd_flags |= (1u << fd0) | (1u << fd1);
        int pair[2] = { fd0, fd1 }; u_memcpy(m->data, pair, sizeof(pair)); m->len = sizeof(pair); m->value = 0; return;
    }
    if (m->op == NOS_VFS_OPEN || m->op == NOS_VFS_MKDIR || m->op == NOS_VFS_UNLINK || m->op == NOS_VFS_RMDIR || m->op == NOS_VFS_RENAME || m->op == NOS_VFS_MOUNT) {
        PATH_BUFFER(path, NOS_PATH_MAX); if (PATH_MISSING(path)) { m->value = -NOS_ENOMEM; return; }
        size_t path_len = payload.len;
        if (m->op == NOS_VFS_RENAME) { path_len = 0; while (path_len < payload.len && payload.bytes[path_len]) path_len++; path_len++; }
        int rc = 0;
        if (payload.resolved) u_memcpy(path, payload.bytes, path_len);
        else rc = resolve_path(t, AT_CURRENT_DIRECTORY, payload.bytes, path_len, path);
        if (rc) { m->value = rc; return; }
        int rc_follow = follow_links(path, m->op == NOS_VFS_OPEN && !(m->arg & NOS_O_NOFOLLOW));
        if (rc_follow) { m->value = rc_follow; return; }
        int node = lookup(path);
        if (m->op == NOS_VFS_OPEN && node >= 0 && NODE_TYPE(&nodes[node]) == MODE_LINK) { m->value = -40; return; }
        if (node < 0 && (m->op == NOS_VFS_MKDIR || m->op == NOS_VFS_UNLINK || m->op == NOS_VFS_RMDIR || m->op == NOS_VFS_RENAME)) {
            unsigned kind; uint64_t size;
            if (archive_lookup(path, &kind, &size) >= 0) { m->value = m->op == NOS_VFS_MKDIR ? -NOS_EEXIST : -30; return; }
        }
        if (m->op == NOS_VFS_MOUNT) { if (m->process != 1) { m->value = -NOS_EPERM; return; } m->value = m->arg == 1 ? disk_mount(path) : m->arg == 2 || m->arg == 3 ? fat_mount(path, m->arg == 3) : -NOS_ENOTSUP; return; }
        if (m->op == NOS_VFS_MKDIR) { m->value = node >= 0 ? -NOS_EEXIST : create(path, MODE_DIR | (0777 & ~t->mask)); if (m->value >= 0) m->value = 0; return; }
        if (m->op == NOS_VFS_UNLINK || m->op == NOS_VFS_RMDIR) {
            if (node < 0) { m->value = node; return; }
            struct vnode *n = &nodes[node];
            if (backend(path) || node == 0 || (disk_mounted && u_streq(path, mount_path)) || (fat_mounted && u_streq(path, fat_path))) { m->value = -NOS_EACCES; return; }
            if (m->op == NOS_VFS_UNLINK && NODE_TYPE(n) == MODE_DIR) { m->value = -NOS_EISDIR; return; }
            if (m->op == NOS_VFS_RMDIR && NODE_TYPE(n) != MODE_DIR) { m->value = -NOS_ENOTDIR; return; }
            for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].used && nodes[i].linked && child_path(nodes[i].name, path)) { m->value = -NOS_ENOTEMPTY; return; }
            int parent = parent_of(path); (void)parent;
            rc = permission_parent(path); if (!rc && parent >= 0) rc = permission_sticky((unsigned)parent, (unsigned)node);
            if (rc) { m->value = rc; return; }
#ifdef NOS_LINUX_COMPAT
            int64_t now; rc = permission_clock(&now); if (rc) { m->value = rc; return; }
#endif
            if (n->persistent == 2) { rc = nos_fat_unlink(fat_relative(path)); if (rc) { m->value = rc; return; } }
#ifdef NOS_LINUX_COMPAT
            permission_touch((unsigned)parent, 6, now); permission_touch((unsigned)node, 4, now);
#endif
            int entry = lookup_entry(path); remove_entry((unsigned)entry); m->value = 0; return;
        }
        if (m->op == NOS_VFS_RENAME) {
            PATH_BUFFER(dest, NOS_PATH_MAX); if (PATH_MISSING(dest)) { m->value = -NOS_ENOMEM; return; }
            if (payload.resolved) u_memcpy(dest, payload.bytes + path_len, payload.len - path_len);
            else rc = resolve_path(t, AT_CURRENT_DIRECTORY, payload.bytes + path_len, payload.len - path_len, dest);
            if (rc || node < 0) { m->value = rc ? rc : node; return; }
            int source_parent = parent_of(path), target_parent = parent_of(dest);
            rc = permission_parent(path); if (!rc) rc = permission_parent(dest);
            if (!rc && source_parent >= 0) rc = permission_sticky((unsigned)source_parent, (unsigned)node);
            int victim = lookup(dest);
            if (!rc && victim >= 0 && target_parent >= 0) rc = permission_sticky((unsigned)target_parent, (unsigned)victim);
            if (!rc && NODE_TYPE(&nodes[node]) == MODE_DIR && source_parent != target_parent) rc = permission_node((unsigned)node, 2);
            if (rc) { m->value = rc; return; }
            if (u_streq(path, dest) || (lookup(dest) >= 0 && lookup(dest) == node)) { m->value = 0; return; }
            if (node == 0 || backend(path) || backend(dest) || child_path(dest, path) || (disk_mounted && u_streq(path, mount_path)) || (fat_mounted && u_streq(path, fat_path))) { m->value = -NOS_EACCES; return; }
            int parent = parent_of(dest); if (parent < 0) { m->value = parent; return; }
            if (nodes[parent].persistent != nodes[node].persistent) { m->value = -18; return; }
            int existing = lookup(dest);
            if (existing >= 0) {
                struct vnode *n = &nodes[existing];
                if ((NODE_TYPE(n) == MODE_DIR) != (NODE_TYPE(&nodes[node]) == MODE_DIR)) { m->value = NODE_TYPE(n) == MODE_DIR ? -NOS_EISDIR : -NOS_ENOTDIR; return; }
                for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].used && nodes[i].linked && child_path(nodes[i].name, dest)) { m->value = -NOS_ENOTEMPTY; return; }
            }
            size_t old = u_strlen(path), replacement = u_strlen(dest);
            size_t name_limit = nodes[node].persistent ? 64u : NOS_PATH_MAX;
            if (replacement >= name_limit) { m->value = -NOS_ENAMETOOLONG; return; }
            for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].used && nodes[i].linked && child_path(nodes[i].name, path) && replacement + u_strlen(nodes[i].name + old) >= name_limit) { m->value = -NOS_ENAMETOOLONG; return; }
#ifdef NOS_LINUX_COMPAT
            int64_t now; rc = permission_clock(&now); if (rc) { m->value = rc; return; }
#endif
            if (nodes[node].persistent == 2) { if (existing >= 0) { m->value = -NOS_EEXIST; return; } rc = nos_fat_rename(fat_relative(path), fat_relative(dest)); if (rc) { m->value = rc; return; } }
#ifdef NOS_LINUX_COMPAT
            permission_touch((unsigned)source_parent, 6, now); permission_touch((unsigned)target_parent, 6, now);
            permission_touch((unsigned)node, 4, now); if (existing >= 0) permission_touch((unsigned)existing, 4, now);
#endif
            if (existing >= 0) remove_entry((unsigned)lookup_entry(dest));
            int source_entry = lookup_entry(path);
            for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].used && nodes[i].linked && ((int)i == source_entry || child_path(nodes[i].name, path))) {
                PATH_SCOPE;
                PATH_BUFFER(renamed, NOS_PATH_MAX); if (PATH_MISSING(renamed)) { m->value = -NOS_ENOMEM; return; }
                u_memcpy(renamed, dest, replacement); size_t suffix = u_strlen(nodes[i].name + old);
                u_memcpy(renamed + replacement, nodes[i].name + old, suffix + 1); u_memcpy(nodes[i].name, renamed, replacement + suffix + 1);
            }
            m->value = 0; return;
        }
        if (m->arg < 0 || (uintptr_t)m->arg > 0xffffffffu) { m->value = -NOS_EINVAL; return; }
        unsigned flags = (unsigned)m->arg;
        if (flags & ~(NOS_O_RDWR | NOS_O_WRONLY | NOS_O_CREAT | NOS_O_TRUNC | NOS_O_APPEND | NOS_O_NONBLOCK | NOS_O_DIRECTORY | NOS_O_NOFOLLOW | NOS_O_CLOEXEC | NOS_O_EXCL) || (flags & 3) == 3) { m->value = -NOS_EINVAL; return; }
        int fd = free_fd(t), desc = free_description();
        if (fd < 0 || desc < 0) { m->value = -NOS_EMFILE; return; }
        unsigned archive_kind = 0; uint64_t archive_size = 0;
        int archive_index = node < 0 ? archive_lookup(path, &archive_kind, &archive_size) : -NOS_ENOENT;
        if (archive_index >= 0) {
            int permitted = permission_path(path, 4); if (permitted) { m->value = permitted; return; }
            if ((flags & NOS_O_CREAT) && (flags & NOS_O_EXCL)) { m->value = -NOS_EEXIST; return; }
            if ((flags & 3) || (flags & NOS_O_TRUNC)) { m->value = -30; return; }
            if ((flags & NOS_O_DIRECTORY) && archive_kind != ARCHIVE_DIRECTORY) { m->value = -NOS_ENOTDIR; return; }
            opened[desc] = (struct description){ .node = (unsigned)archive_index, .kind = archive_kind, .size = archive_size, .flags = flags, .refs = 1 };
            t->fd[fd] = desc + 1; t->fd_flags &= ~(1u << fd);
            if (flags & NOS_O_CLOEXEC) t->fd_flags |= 1u << fd;
            m->value = fd; return;
        }
        if (node >= 0 && (flags & NOS_O_CREAT) && (flags & NOS_O_EXCL)) { m->value = -NOS_EEXIST; return; }
        unsigned created_now = node < 0 && (flags & NOS_O_CREAT);
        if (created_now) node = create(path, MODE_FILE | (create_mode & ~t->mask));
        if (node < 0) { m->value = node; return; }
        if (!created_now) { unsigned access = (flags & 3) == NOS_O_RDWR ? 6u : (flags & 3) == NOS_O_WRONLY ? 2u : 4u;
            int permitted = permission_node((unsigned)node, access); if (permitted) { m->value = permitted; return; } }
        if ((flags & NOS_O_DIRECTORY) && NODE_TYPE(&nodes[node]) != MODE_DIR) { m->value = -NOS_ENOTDIR; return; }
        if (NODE_TYPE(&nodes[node]) == MODE_DIR && ((flags & 3) || (flags & NOS_O_TRUNC))) { m->value = -NOS_EISDIR; return; }
        if ((flags & 3) && backend(path) == 2) { m->value = -NOS_EACCES; return; }
        if ((flags & NOS_O_TRUNC) && (flags & 3)) {
#ifdef NOS_LINUX_COMPAT
            int64_t now; rc = permission_clock(&now); if (rc) { m->value = rc; return; }
#endif
            if (nodes[node].persistent == 2) { rc = nos_fat_write(fat_relative(path), "", 0); if (rc) { m->value = rc; return; } }
            nodes[node].size = 0;
#ifdef NOS_LINUX_COMPAT
            permission_touch((unsigned)node, 6, now); if (subject.credentials.euid) nodes[node].mode &= ~06000u;
#endif
        }
        opened[desc] = (struct description){ .node = node, .flags = flags, .refs = 1, .kind = NODE_TYPE(&nodes[node]) == MODE_CHAR ? 1u : 0u };
        nodes[node].refs++; t->fd[fd] = desc + 1; t->fd_flags &= ~(1u << fd);
        if (flags & NOS_O_CLOEXEC) t->fd_flags |= 1u << fd;
        m->value = fd; return;
    }
    if (m->arg < 0 || (uintptr_t)m->arg >= FDS || !t->fd[m->arg]) { m->value = -NOS_EBADF; return; }
    unsigned fd = (unsigned)m->arg, desc = t->fd[fd] - 1;
    struct description *d = &opened[desc];
    switch (m->op) {
    case NOS_VFS_DUP: {
        unsigned close_on_exec = (unsigned char)m->data[0];
        if (close_on_exec > 1) { m->value = -NOS_EINVAL; break; }
        intptr_t target = m->value;
        if (target < 0) {
#ifdef NOS_LINUX_COMPAT
            /* Plain dup and F_DUPFD differ when the soft limit is zero:
             * dup exhausts the table; F_DUPFD rejects its minimum. */
            if ((target != -1 && target != -2) || (target == -2 && m->len) ||
                (target == -1 && m->len >= descriptor_limit)) { m->value = -NOS_EINVAL; break; }
#else
            if (m->len >= descriptor_limit) { m->value = -NOS_EINVAL; break; }
#endif
            target = -NOS_EMFILE;
            for (unsigned i = (unsigned)m->len; i < descriptor_limit; i++) if (!t->fd[i]) { target = (intptr_t)i; break; }
        }
        if (target < 0 || (uintptr_t)target >= FDS) { m->value = target < 0 ? target : -NOS_EBADF; break; }
        if ((unsigned)target == fd && close_on_exec) { m->value = -NOS_EINVAL; break; }
        /* An existing high descriptor survives a lowered soft limit, including
         * dup2(fd, fd). A real replacement checks admission before any close. */
        if ((unsigned)target != fd && (uintptr_t)target >= descriptor_limit) { m->value = -NOS_EBADF; break; }
        if ((unsigned)target != fd) { if (t->fd[target]) close_descriptor(t, (unsigned)target); t->fd[target] = desc + 1; t->fd_flags = (t->fd_flags & ~(1u << target)) | (close_on_exec << target); d->refs++; }
        m->value = target; break;
    }
    case 30: m->value = (t->fd_flags >> fd) & 1u; break;
    case 31:
        if (m->value < 0 || m->value > 1) m->value = -NOS_EINVAL;
        else { t->fd_flags = (t->fd_flags & ~(1u << fd)) | ((unsigned)m->value << fd); m->value = 0; }
        break;
    case NOS_VFS_FCHDIR: {
#ifdef NOS_LINUX_COMPAT
        m->value = cwd_set_description(t, d);
#else
        char path[NOS_PATH_MAX]; m->value = !description_directory(d) ? -NOS_ENOTDIR : description_path(d, path);
        if (!m->value) m->value = permission_path(path, 1);
        if (!m->value) u_memcpy(t->cwd, path, u_strlen(path) + 1);
#endif
        break;
    }
    case NOS_VFS_STAT64: {
        struct nos_stat64 st; m->value = full_stat(d, &st);
        if (!m->value) { u_memcpy(m->data, &st, sizeof(st)); m->len = sizeof(st); }
        break;
    }
    case NOS_VFS_GETDENTS: get_directory(m, d); break;
    case NOS_VFS_PREAD: case NOS_VFS_PWRITE: {
        uint64_t offset; u_memcpy(&offset, m->data, sizeof(offset));
        if (offset > INT64_MAX) { m->value = -NOS_EINVAL; break; }
        if (d->kind == 1 || d->kind == 2) { m->value = -29; break; }
        if (m->op == NOS_VFS_PWRITE && m->len < 8) { m->value = -NOS_EINVAL; break; }
        struct description positioned = *d; positioned.offset = offset;
        if (m->op == NOS_VFS_PWRITE) positioned.flags &= ~NOS_O_APPEND;
        if (m->op == NOS_VFS_PREAD) {
#ifdef NOS_LINUX_COMPAT
            if (m->bulk_out_len) {
                if (m->len != sizeof(offset) || m->bulk_out_len > sizeof(positioned_read_scratch)) { m->value = -NOS_EINVAL; break; }
                intptr_t count = read_data(&positioned, positioned_read_scratch, m->bulk_out_len);
                if (count > 0) {
                    int rc = path_transfer(m, positioned_read_scratch, (size_t)count, NOS_IPC_STORE);
                    if (rc) count = rc;
                }
                m->len = 0; m->value = count; break;
            }
#endif
            m->value = read_data(&positioned, m->data, m->len); if (m->value >= 0) m->len = (size_t)m->value;
        } else m->value = write_data(&positioned, m->data + 8, m->len - 8);
        break;
    }
    case NOS_VFS_GETFLAGS: m->value = d->flags & (3u | NOS_O_APPEND | NOS_O_NONBLOCK); break;
    case NOS_VFS_SETFLAGS:
        if (m->value < 0 || ((uintptr_t)m->value & ~(3u | NOS_O_APPEND | NOS_O_NONBLOCK))) m->value = -NOS_EINVAL;
        else { d->flags = (d->flags & ~(NOS_O_APPEND | NOS_O_NONBLOCK)) | ((unsigned)m->value & (NOS_O_APPEND | NOS_O_NONBLOCK)); m->value = 0; }
        break;
    case NOS_VFS_CLOSE: close_descriptor(t, fd); m->value = 0; break;
    case NOS_VFS_READ: case NOS_VFS_WRITE: {
        uintptr_t allowed_io = NOS_IO_DONTWAIT;
#ifdef NOS_LINUX_COMPAT
        if (m->op == NOS_VFS_WRITE) allowed_io |= NOS_IO_WRITE_LARGE;
#endif
        if (m->value < 0 || ((uintptr_t)m->value & ~allowed_io)) { m->value = -NOS_EINVAL; break; }
        if ((m->op == NOS_VFS_READ && (d->flags & 3) == NOS_O_WRONLY) ||
            (m->op == NOS_VFS_WRITE && !(d->flags & 3))) { m->value = -NOS_EBADF; break; }
#ifdef NOS_LINUX_COMPAT
        if (d->kind == 2 && m->op == NOS_VFS_READ && m->bulk_out_len && m->bulk_out_len != m->len) {
            m->value = -NOS_EINVAL; break;
        }
        if (m->op == NOS_VFS_WRITE && m->bulk_in_len) {
            if (m->len || m->bulk_in_len > sizeof(pipe_scratch)) { m->value = -NOS_EINVAL; break; }
            if (d->kind != 2) {
                int rc = pipe_fetch(m, pipe_scratch, m->bulk_in_len);
                m->value = rc ? rc : write_data(d, pipe_scratch, m->bulk_in_len);
                break;
            }
        }
#endif
        if (d->kind == 2 && pipe_count(m)) {
            struct pipe_buffer *pipe = &pipes[d->pipe];
#ifdef NOS_LINUX_COMPAT
            if (m->op == NOS_VFS_WRITE && (m->value & NOS_IO_WRITE_LARGE) && pipe->readers &&
                ((d->flags & NOS_O_NONBLOCK) || (m->value & NOS_IO_DONTWAIT)) &&
                PIPE_BYTES > pipe->size && pipe_count(m) > PIPE_BYTES - pipe->size)
                pipe_limit_count(m, PIPE_BYTES - pipe->size);
#endif
            if (((d->flags & NOS_O_NONBLOCK) || (m->value & NOS_IO_DONTWAIT)) &&
                (m->op == NOS_VFS_READ ? !pipe->size && pipe->writers :
                    pipe->readers && pipe_count(m) > PIPE_BYTES - pipe->size)) {
                m->value = -NOS_EAGAIN; break;
            }
            int rc = pipe_defer(m, desc); if (rc) m->value = rc; break;
        }
        m->value = m->op == NOS_VFS_READ ? read_data(d, m->data, m->len) : write_data(d, m->data, m->len);
        if (m->op == NOS_VFS_READ && m->value >= 0) m->len = (size_t)m->value;
        break;
    }
    case NOS_VFS_SEEK: {
        if (d->kind == 1 || d->kind == 2) { m->value = -29; break; }
        if (m->len > 2) { m->value = -NOS_EINVAL; break; }
        uint64_t size = d->kind == ARCHIVE_FILE ? d->size : d->kind == ARCHIVE_DIRECTORY ? 0 : nodes[d->node].size;
        uint64_t base = m->len == 0 ? 0 : m->len == 1 ? d->offset : size;
        if ((m->value < 0 && (uint64_t)(-(m->value + 1)) + 1 > base) ||
            (m->value >= 0 && (uint64_t)m->value > INT64_MAX - base)) { m->value = -NOS_EINVAL; break; }
        uint64_t result = m->value < 0 ? base - ((uint64_t)(-(m->value + 1)) + 1) : base + (uint64_t)m->value;
        if (result > INTPTR_MAX) { m->value = -NOS_EOVERFLOW; break; }
        d->offset = result; m->value = (intptr_t)result; break;
    }
    case NOS_VFS_STAT: {
        struct nos_stat st;
        u_memset(&st, 0, sizeof(st));
        st.size = d->kind == ARCHIVE_FILE ? (size_t)d->size : d->kind ? 0 : nodes[d->node].size;
        st.mode = d->kind == ARCHIVE_FILE ? MODE_FILE : d->kind == ARCHIVE_DIRECTORY ? MODE_DIR : d->kind == 2 ? MODE_FIFO : d->kind == 1 ? MODE_CHAR : nodes[d->node].mode;
        u_memcpy(m->data, &st, sizeof(st)); m->len = sizeof(st); m->value = 0; break;
    }
    case NOS_VFS_READDIR: {
        char name[VFS_DIRENT_NAME_BYTES]; struct nos_stat64 st;
        int rc = m->value < 0 ? -NOS_EINVAL : directory_entry(d, (uint64_t)m->value + 2, name, &st);
        m->value = rc; if (rc > 0) { path_output(m, name, u_strlen(name) + 1, 1); if (m->value >= 0) m->value = 1; }
        break;
    }
    default: m->value = -NOS_ENOSYS; break;
    }
}
static int initialize_namespace(void)
{
#ifdef NOS_LINUX_COMPAT
    int storage = path_storage_initialize(); if (storage) return storage;
    for (unsigned i = 0; i < NODE_COUNT; i++) { nodes[i].name = path_storage + i * NOS_PATH_MAX; nodes[i].name[0] = 0; }
#endif
    const char *initial[] = { "/", "/tmp", "/dev", "/proc", "/dev/console", "/proc/version"
#ifdef NOS_LINUX_COMPAT
        , "/dev/shm", "/proc/sys", "/proc/sys/kernel", "/proc/sys/kernel/ngroups_max"
#endif
    };
    for (unsigned i = 0; i < sizeof(initial) / sizeof(initial[0]); i++) {
        u_memcpy(nodes[i].name, initial[i], u_strlen(initial[i]) + 1);
        nodes[i].used = nodes[i].linked = 1;
#ifdef NOS_LINUX_COMPAT
        nodes[i].mode = i < 4 || i == 6 || i == 7 || i == 8 ? MODE_DIR | (i == 1 || i == 6 ? 01777 : 0755) : i == 4 ? MODE_CHAR | 0666 : MODE_FILE | 0444;
#else
        nodes[i].mode = i < 4 ? MODE_DIR | 0777 : i == 4 ? MODE_CHAR | 0666 : MODE_FILE | 0444;
#endif
    }
#ifdef NOS_LINUX_COMPAT
    /* Generated proc data advertises the real implemented kernel limit. */
    const char *groups_max = "32\n";
    _Static_assert(NOS_NGROUPS_MAX == 32, "update generated proc group limit");
    nodes[9].size = 3; u_memcpy(nodes[9].bytes, groups_max, 3);
#endif
    const char *version = "NOS microkernel\n";
    nodes[5].size = u_strlen(version); u_memcpy(nodes[5].bytes, version, nodes[5].size);
    for (unsigned i = 0; i < 3; i++) opened[i] = (struct description){ .node = 4, .kind = 1, .flags = i ? NOS_O_WRONLY : NOS_O_RDONLY };
    for (unsigned i = 0; i < FD_TABLE_COUNT; i++) fds[i].pid = -1;
    return 0;
}
void nos_vfs_server(uintptr_t unused)
{
    (void)unused; if (initialize_namespace()) nos_syscall(NOS_SYS_EXIT, NOS_ENOMEM, 0, 0); nos_uart_init();
    for (;;) {
        struct nos_message message;
        if (nos_syscall(NOS_SYS_RECV, 0, (uintptr_t)&message, 0)) nos_syscall(NOS_SYS_EXIT, 1, 0, 0);
        uintptr_t sender = (uintptr_t)message.sender;
        if (message.op == NOS_IPC_PROCESS_FORK) {
            if (inherit_files((intptr_t)message.process, message.value)) nos_syscall(NOS_SYS_EXIT, NOS_ENOSPC, 0, 0);
            service_complete(); continue;
        }
        if (message.op == NOS_IPC_PROCESS_EXEC) { exec_files((intptr_t)message.process); service_complete(); continue; }
        if (message.op == NOS_IPC_PEER_CANCEL) { pipe_cancel(message.sender, message.token, 1); service_complete(); continue; }
        if (message.op == NOS_IPC_PEER_EXIT) {
            pipe_cancel(message.sender, 0, 0);
#ifndef NOS_LINUX_COMPAT
            if (!message.value) for (unsigned i = 0; i < FD_TABLE_COUNT; i++)
                if (fds[i].pid == (intptr_t)message.process) release_table(&fds[i]);
#endif
            service_complete(); continue;
        }
        defer_reply = 0; service(&message);
        if (!defer_reply) {
            intptr_t rc = nos_syscall(NOS_SYS_REPLY, sender, (uintptr_t)&message, 0);
#ifdef NOS_LINUX_COMPAT
            if (rc && message.op == NOS_VFS_SHARED_FILE && !nos_pointer_is_error(message.value)) {
                struct nos_shared_request request = { .handle = (uintptr_t)message.value };
                nos_syscall(NOS_SYS_SHARED, NOS_SHARED_RELEASE, (uintptr_t)&request, 0);
            }
#else
            (void)rc;
#endif
        }
        service_complete();
    }
}
