/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
#include "../drivers/devices.h"
#include "../fatfs/adapter.h"
#define NODE_COUNT 16u
#define FILE_BYTES 512u
#define FDS 11u
#define OPEN_COUNT (NOS_TASKS * FDS)
#define PIPE_COUNT 2u
#define PIPE_BYTES 256u
#define MODE_DIR 0040000u
#define MODE_FILE 0100000u
#define MODE_CHAR 0020000u
#define MODE_FIFO 0010000u
#define NO_NODE 0xffffu
struct vnode { char name[64], bytes[FILE_BYTES]; size_t size; unsigned mode, used, linked, refs, persistent; };
struct description { unsigned node, offset, flags, refs, kind, pipe; };
struct fd_table { intptr_t pid; unsigned short fd[FDS]; };
struct pipe_buffer { char bytes[PIPE_BYTES]; unsigned head, size, readers, writers; };
static struct vnode nodes[NODE_COUNT];
static struct description opened[OPEN_COUNT];
static struct fd_table fds[NOS_TASKS];
static struct pipe_buffer pipes[PIPE_COUNT];
static unsigned disk_mounted;
static unsigned fat_mounted;
static char fat_path[64];
static unsigned disk_generation;
static char mount_path[64];
static int prefix(const char *s, const char *p)
{ while (*p) if (*s++ != *p++) return 0; return 1; }
static int child_path(const char *s, const char *p)
{ size_t n = u_strlen(p); return prefix(s, p) && (n == 1 || s[n] == '/'); }
static int backend(const char *p)
{ return child_path(p, "/dev") || u_streq(p, "/dev") ? 1 : child_path(p, "/proc") || u_streq(p, "/proc") ? 2 : 0; }
static int normalize(const char *src, size_t len, char *dst)
{
    if (!len || len > 64 || src[0] != '/' || src[len - 1]) return -NOS_EINVAL;
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
        if (end + n >= 64) return -NOS_ENAMETOOLONG;
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
static int lookup(const char *path)
{
    for (unsigned i = 0; i < NODE_COUNT; i++)
        if (nodes[i].used && nodes[i].linked && path_equal(path, nodes[i].name, nodes[i].persistent == 2)) return (int)i;
    return -NOS_ENOENT;
}
static int parent_of(const char *path)
{
    char parent[64]; size_t n = u_strlen(path);
    while (n > 1 && path[n - 1] != '/') n--;
    if (n > 1) n--;
    u_memcpy(parent, path, n); parent[n] = 0;
    int p = lookup(parent);
    return p < 0 ? p : nodes[p].mode == MODE_DIR ? p : -NOS_ENOTDIR;
}
static int create(const char *path, unsigned mode)
{
    if (u_streq(path, "/")) return -NOS_EEXIST;
    int p = parent_of(path); if (p < 0) return p;
    if (backend(path)) return -NOS_EACCES;
    for (unsigned i = 0; i < NODE_COUNT; i++) if (!nodes[i].used) {
        if (nodes[p].persistent == 2) { int rc = mode == MODE_DIR ? nos_fat_mkdir(fat_relative(path)) : nos_fat_write(fat_relative(path), "", 0); if (rc) return rc; }
        u_memset(&nodes[i], 0, sizeof(nodes[i]));
        u_memcpy(nodes[i].name, path, u_strlen(path) + 1);
        nodes[i].mode = mode; nodes[i].used = nodes[i].linked = 1;
        nodes[i].persistent = nodes[p].persistent;
        return (int)i;
    }
    return -NOS_ENOSPC;
}
static void drop_description(unsigned d)
{
    if (d >= OPEN_COUNT || !opened[d].refs) return;
    if (!--opened[d].refs && d >= 3) {
        struct description *o = &opened[d];
        if (o->kind == 2) { struct pipe_buffer *p = &pipes[o->pipe]; if (o->flags & 3) p->writers--; else p->readers--; }
        else if (o->node < NODE_COUNT) {
            struct vnode *n = &nodes[o->node]; if (n->refs) n->refs--;
            if (!n->linked && !n->refs) u_memset(n, 0, sizeof(*n));
        }
        u_memset(o, 0, sizeof(*o));
    }
}
static void release_table(struct fd_table *t)
{ for (unsigned i = 0; i < FDS; i++) if (t->fd[i]) { drop_description(t->fd[i] - 1); t->fd[i] = 0; } t->pid = -1; }
static struct fd_table *table_for(intptr_t pid)
{
    struct fd_table *t = 0;
    for (unsigned i = 0; i < NOS_TASKS; i++) if (fds[i].pid == pid) return &fds[i];
    for (unsigned i = 0; i < NOS_TASKS; i++) if (fds[i].pid < 0) { t = &fds[i]; break; }
    if (!t) return 0;
    {
        release_table(t); t->pid = pid;
        for (unsigned i = 0; i < 3; i++) { t->fd[i] = i + 1; opened[i].refs++; }
    }
    return t;
}
static int free_fd(struct fd_table *t)
{ for (unsigned i = 0; i < FDS; i++) if (!t->fd[i]) return (int)i; return -NOS_EMFILE; }
static int free_description(void)
{ for (unsigned i = 3; i < OPEN_COUNT; i++) if (!opened[i].refs) return (int)i; return -NOS_EMFILE; }
static intptr_t read_data(struct description *d, char *data, size_t count)
{
    if ((d->flags & 3) == NOS_O_WRONLY) return -NOS_EBADF;
    if (!count) return 0;
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
    if (n->mode == MODE_DIR) return -NOS_EISDIR;
    if (d->offset >= n->size) return 0;
    if (count > n->size - d->offset) count = n->size - d->offset;
    u_memcpy(data, n->bytes + d->offset, count); d->offset += count; return (intptr_t)count;
}
static intptr_t write_data(struct description *d, const char *data, size_t count)
{
    if (!(d->flags & 3)) return -NOS_EBADF;
    if (!count) return 0;
    if (d->kind == 1) return nos_syscall(NOS_SYS_WRITE, (uintptr_t)data, count, 0);
    if (d->kind == 2) {
        struct pipe_buffer *p = &pipes[d->pipe];
        if (!p->readers) return -32; /* EPIPE, no unsupported signal emulation. */
        if (count > PIPE_BYTES - p->size) return -NOS_EAGAIN;
        for (size_t i = 0; i < count; i++) p->bytes[(p->head + p->size + i) % PIPE_BYTES] = data[i];
        p->size += count; return (intptr_t)count;
    }
    struct vnode *n = &nodes[d->node];
    if (n->mode == MODE_DIR) return -NOS_EISDIR;
    if (backend(n->name) == 2) return -NOS_EACCES;
    if (d->flags & 0x400u) d->offset = (unsigned)n->size;
    if (d->offset >= FILE_BYTES) return -NOS_ENOSPC;
    if (count > FILE_BYTES - d->offset) count = FILE_BYTES - d->offset;
    if (d->offset > n->size) u_memset(n->bytes + n->size, 0, d->offset - n->size);
    u_memcpy(n->bytes + d->offset, data, count); d->offset += count;
    if (d->offset > n->size) n->size = d->offset;
    if (n->persistent == 2 && n->linked) { int rc = nos_fat_write(fat_relative(n->name), n->bytes, n->size); if (rc) return rc; }
    return (intptr_t)count;
}
#include "pipe_wait.h"
static unsigned crc32(unsigned crc, const void *buffer, size_t count)
{
    const unsigned char *p = buffer;
    while (count--) { crc ^= *p++; for (unsigned j = 0; j < 8; j++) crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u))); }
    return crc;
}
#define BANK_SECTORS (1u + 2u * NODE_COUNT)
struct disk_header { uint32_t magic, version, generation, count, crc, header_crc; };
struct disk_node { uint32_t mode, size; char name[64]; };
static int disk_bank_check(unsigned bank, struct disk_header *header)
{
    unsigned char sector[512];
    int rc = nos_block_read(bank * BANK_SECTORS, sector); if (rc) return rc;
    u_memcpy(header, sector, sizeof(*header));
    if (header->magic != 0x4e4f5346u || header->version != 2 || header->count > NODE_COUNT || header->header_crc != ~crc32(~0u, header, offsetof(struct disk_header, header_crc))) return -NOS_EINVAL;
    unsigned crc = ~0u;
    for (unsigned i = 0; i < NODE_COUNT * 2; i++) { rc = nos_block_read(bank * BANK_SECTORS + 1 + i, sector); if (rc) return rc; crc = crc32(crc, sector, 512); }
    return (~crc == header->crc) ? 0 : -NOS_EIO;
}
static int disk_sync(void)
{
    if (!disk_mounted) return -NOS_ENOTSUP;
    if (disk_generation == UINT32_MAX) return -NOS_EOVERFLOW;
    unsigned bank = (disk_generation + 1) & 1u, crc = ~0u, count = 0;
    unsigned char sector[512];
    for (unsigned i = 0; i < NODE_COUNT; i++) {
        struct vnode *n = &nodes[i]; u_memset(sector, 0, sizeof(sector));
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
    if (disk_mounted || fat_mounted) return -NOS_EBUSY;
    if (backend(path) || u_streq(path, "/")) return -NOS_EACCES;
    int root = lookup(path);
    if (root >= 0 && nodes[root].mode != MODE_DIR) return -NOS_ENOTDIR;
    for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].used && nodes[i].linked && child_path(nodes[i].name, path)) return -NOS_ENOTEMPTY;
    int rc = nos_block_init(); if (rc) return rc;
    struct disk_header headers[2]; int valid[2];
    for (unsigned i = 0; i < 2; i++) valid[i] = !disk_bank_check(i, &headers[i]);
    unsigned bank = valid[1] && (!valid[0] || headers[1].generation > headers[0].generation) ? 1 : 0;
    struct disk_node entries[NODE_COUNT]; unsigned char sector[512]; unsigned count = 0, roots = 0, free_nodes = 0;
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
        for (unsigned i = 0; i < NODE_COUNT; i++) {
            rc = nos_block_read(bank * BANK_SECTORS + 1 + 2 * i, sector); if (rc) return rc;
            u_memcpy(&entries[i], sector, sizeof(entries[i])); struct disk_node *entry = &entries[i];
            if (!entry->mode) continue;
            if (entry->size > FILE_BYTES || entry->name[63] || !(entry->mode == MODE_FILE || entry->mode == MODE_DIR) || (entry->mode == MODE_DIR && entry->size)) return -NOS_EINVAL;
            char canonical[64]; rc = normalize(entry->name, u_strlen(entry->name) + 1, canonical);
            if (rc || !u_streq(canonical, entry->name) || !(u_streq(entry->name, path) || child_path(entry->name, path))) return -NOS_EACCES;
            if (u_streq(entry->name, path)) { if (entry->mode != MODE_DIR) return -NOS_ENOTDIR; roots++; }
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
        for (unsigned i = 0; i < NODE_COUNT; i++) {
            struct disk_node *entry = &entries[i]; if (!entry->mode || u_streq(entry->name, path)) continue;
            int slot = -1; for (unsigned j = 0; j < NODE_COUNT; j++) if (!nodes[j].used) { slot = (int)j; break; }
            if (slot < 0) { rc = -NOS_ENOSPC; goto rollback; }
            rc = nos_block_read(bank * BANK_SECTORS + 2 + 2 * i, sector); if (rc) goto rollback;
            struct vnode *n = &nodes[slot]; u_memset(n, 0, sizeof(*n));
            u_memcpy(n->name, entry->name, 64); n->size = entry->size; n->mode = entry->mode; n->used = n->linked = n->persistent = 1;
            u_memcpy(n->bytes, sector, n->size); installed |= 1u << slot;
        }
        for (unsigned i = 0; i < NODE_COUNT; i++) if ((installed & (1u << i)) && parent_of(nodes[i].name) < 0) { rc = -NOS_EINVAL; goto rollback; }
        disk_generation = headers[bank].generation;
    }
    u_memcpy(mount_path, path, u_strlen(path) + 1); disk_mounted = 1; return 0;
rollback:
    for (unsigned i = 0; i < NODE_COUNT; i++) if (installed & (1u << i)) u_memset(&nodes[i], 0, sizeof(nodes[i]));
    nodes[root].persistent = 0; if (created) u_memset(&nodes[root], 0, sizeof(nodes[root])); return rc;
}
static int fat_import(const char *path, unsigned depth)
{
    if (depth > 8) return -NOS_EINVAL;
    for (unsigned index = 0;; index++) {
        char name[64], full[64]; unsigned mode; size_t size;
        int rc = nos_fat_list(fat_relative(path), index, name, &mode, &size);
        if (rc <= 0) return rc;
        if (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))) continue;
        size_t n = u_strlen(path), extra = u_strlen(name);
        if (n + 1 + extra >= 64 || size > FILE_BYTES) return -NOS_ENOTSUP;
        u_memcpy(full, path, n); full[n++] = '/'; u_memcpy(full + n, name, extra + 1);
        int slot = -1; for (unsigned i = 0; i < NODE_COUNT; i++) if (!nodes[i].used) { slot = (int)i; break; }
        if (slot < 0) return -NOS_ENOSPC;
        struct vnode *node = &nodes[slot]; u_memset(node, 0, sizeof(*node));
        u_memcpy(node->name, full, n + extra + 1); node->mode = mode; node->size = size; node->used = node->linked = 1; node->persistent = 2;
        if (mode == MODE_DIR) rc = fat_import(full, depth + 1);
        else { rc = nos_fat_read(fat_relative(full), node->bytes, size); if (rc >= 0) rc = (size_t)rc == size ? 0 : -NOS_EIO; }
        if (rc) return rc;
    }
}
static int fat_mount(const char *path, int format)
{
    if (disk_mounted || fat_mounted) return -NOS_EBUSY;
    if (backend(path) || u_streq(path, "/")) return -NOS_EACCES;
    int root = lookup(path);
    if (root >= 0 && nodes[root].mode != MODE_DIR) return -NOS_ENOTDIR;
    for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].used && nodes[i].linked && child_path(nodes[i].name, path)) return -NOS_ENOTEMPTY;
    int rc = nos_fat_mount(format); if (rc) return rc;
    unsigned created = root < 0; if (root < 0) root = create(path, MODE_DIR); if (root < 0) return root;
    u_memcpy(fat_path, path, u_strlen(path) + 1); nodes[root].persistent = 2;
    rc = fat_import(path, 0);
    if (rc) {
        for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].persistent == 2 && (int)i != root) u_memset(&nodes[i], 0, sizeof(nodes[i]));
        nodes[root].persistent = 0; if (created) u_memset(&nodes[root], 0, sizeof(nodes[root])); fat_path[0] = 0; return rc;
    }
    fat_mounted = 1; return 0;
}
static void service(struct nos_message *m)
{
    if (m->sender < 0) { m->value = -NOS_EPERM; return; }
    if (m->len > NOS_MSG_DATA) { m->value = -NOS_EINVAL; return; }
    struct fd_table *t = table_for((intptr_t)m->process);
    if (!t) { m->value = -NOS_ENOSPC; return; }
    if (m->op == NOS_VFS_DEVICE_STATS) {
        if (m->arg == 1) { struct nos_uart_stats stats; nos_uart_get_stats(&stats); u_memcpy(m->data, &stats, sizeof(stats)); m->len = sizeof(stats); m->value = 0; return; }
        if (m->arg) { m->value = -NOS_EINVAL; return; }
        struct nos_block_stats stats = { nos_block_interrupts(), nos_block_requests(), nos_block_poll_fallbacks(), nos_block_capacity(), nos_block_version(), 0 };
        u_memcpy(m->data, &stats, sizeof(stats)); m->len = sizeof(stats); m->value = 0; return;
    }
    if (m->op == NOS_VFS_SYNC) { m->value = fat_mounted ? nos_fat_sync() : disk_sync(); return; }
    if (m->op == NOS_VFS_PIPE) {
        if (m->arg < 0 || ((uintptr_t)m->arg & ~NOS_O_NONBLOCK)) { m->value = -NOS_EINVAL; return; }
        unsigned flags = (unsigned)m->arg;
        int fd0 = free_fd(t), fd1 = -1, d0 = free_description(), d1 = -1, pipe = -1;
        if (fd0 >= 0) for (unsigned i = fd0 + 1; i < FDS; i++) if (!t->fd[i]) { fd1 = (int)i; break; }
        if (d0 >= 0) for (unsigned i = d0 + 1; i < OPEN_COUNT; i++) if (!opened[i].refs) { d1 = (int)i; break; }
        for (unsigned i = 0; i < PIPE_COUNT; i++) if (!pipes[i].readers && !pipes[i].writers) { pipe = (int)i; break; }
        if (fd0 < 0 || fd1 < 0 || d0 < 0 || d1 < 0) { m->value = -NOS_EMFILE; return; }
        if (pipe < 0) { m->value = -NOS_ENOSPC; return; }
        u_memset(&pipes[pipe], 0, sizeof(pipes[pipe])); pipes[pipe].readers = pipes[pipe].writers = 1;
        opened[d0] = (struct description){ .refs = 1, .kind = 2, .pipe = pipe, .flags = flags };
        opened[d1] = (struct description){ .refs = 1, .kind = 2, .pipe = pipe, .flags = NOS_O_WRONLY | flags };
        t->fd[fd0] = d0 + 1; t->fd[fd1] = d1 + 1;
        int pair[2] = { fd0, fd1 }; u_memcpy(m->data, pair, sizeof(pair)); m->len = sizeof(pair); m->value = 0; return;
    }
    if (m->op == NOS_VFS_OPEN || m->op == NOS_VFS_MKDIR || m->op == NOS_VFS_UNLINK || m->op == NOS_VFS_RMDIR || m->op == NOS_VFS_RENAME || m->op == NOS_VFS_MOUNT) {
        char path[64]; size_t path_len = m->len;
        if (m->op == NOS_VFS_RENAME) { path_len = 0; while (path_len < m->len && m->data[path_len]) path_len++; path_len++; }
        int rc = normalize(m->data, path_len, path); if (rc) { m->value = rc; return; }
        int node = lookup(path);
        if (m->op == NOS_VFS_MOUNT) { if (m->process != 1) { m->value = -NOS_EPERM; return; } m->value = m->arg == 1 ? disk_mount(path) : m->arg == 2 || m->arg == 3 ? fat_mount(path, m->arg == 3) : -NOS_ENOTSUP; return; }
        if (m->op == NOS_VFS_MKDIR) { m->value = node >= 0 ? -NOS_EEXIST : create(path, MODE_DIR); if (m->value >= 0) m->value = 0; return; }
        if (m->op == NOS_VFS_UNLINK || m->op == NOS_VFS_RMDIR) {
            if (node < 0) { m->value = node; return; }
            struct vnode *n = &nodes[node];
            if (backend(path) || node == 0 || (disk_mounted && u_streq(path, mount_path)) || (fat_mounted && u_streq(path, fat_path))) { m->value = -NOS_EACCES; return; }
            if (m->op == NOS_VFS_UNLINK && n->mode == MODE_DIR) { m->value = -NOS_EISDIR; return; }
            if (m->op == NOS_VFS_RMDIR && n->mode != MODE_DIR) { m->value = -NOS_ENOTDIR; return; }
            for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].used && nodes[i].linked && child_path(nodes[i].name, path)) { m->value = -NOS_ENOTEMPTY; return; }
            if (n->persistent == 2) { rc = nos_fat_unlink(fat_relative(path)); if (rc) { m->value = rc; return; } }
            n->linked = 0; if (!n->refs) u_memset(n, 0, sizeof(*n)); m->value = 0; return;
        }
        if (m->op == NOS_VFS_RENAME) {
            char dest[64]; rc = normalize(m->data + path_len, m->len - path_len, dest);
            if (rc || node < 0) { m->value = rc ? rc : node; return; }
            if (u_streq(path, dest)) { m->value = 0; return; }
            if (node == 0 || backend(path) || backend(dest) || child_path(dest, path) || (disk_mounted && u_streq(path, mount_path)) || (fat_mounted && u_streq(path, fat_path))) { m->value = -NOS_EACCES; return; }
            int parent = parent_of(dest); if (parent < 0) { m->value = parent; return; }
            if (nodes[parent].persistent != nodes[node].persistent) { m->value = -18; return; }
            int existing = lookup(dest);
            if (existing >= 0) {
                struct vnode *n = &nodes[existing];
                if ((n->mode == MODE_DIR) != (nodes[node].mode == MODE_DIR)) { m->value = n->mode == MODE_DIR ? -NOS_EISDIR : -NOS_ENOTDIR; return; }
                for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].used && nodes[i].linked && child_path(nodes[i].name, dest)) { m->value = -NOS_ENOTEMPTY; return; }
            }
            size_t old = u_strlen(path), replacement = u_strlen(dest);
            for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].used && nodes[i].linked && child_path(nodes[i].name, path) && replacement + u_strlen(nodes[i].name + old) >= 64) { m->value = -NOS_ENAMETOOLONG; return; }
            if (nodes[node].persistent == 2) { if (existing >= 0) { m->value = -NOS_EEXIST; return; } rc = nos_fat_rename(fat_relative(path), fat_relative(dest)); if (rc) { m->value = rc; return; } }
            if (existing >= 0) { nodes[existing].linked = 0; if (!nodes[existing].refs) u_memset(&nodes[existing], 0, sizeof(nodes[existing])); }
            for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].used && nodes[i].linked && ((int)i == node || child_path(nodes[i].name, path))) {
                char renamed[64]; u_memcpy(renamed, dest, replacement); size_t suffix = u_strlen(nodes[i].name + old);
                u_memcpy(renamed + replacement, nodes[i].name + old, suffix + 1); u_memcpy(nodes[i].name, renamed, replacement + suffix + 1);
            }
            m->value = 0; return;
        }
        if (m->arg < 0 || (uintptr_t)m->arg > 0xffffffffu) { m->value = -NOS_EINVAL; return; }
        unsigned flags = (unsigned)m->arg;
        if (flags & ~(NOS_O_RDWR | NOS_O_WRONLY | NOS_O_CREAT | NOS_O_TRUNC | NOS_O_APPEND | NOS_O_NONBLOCK) || (flags & 3) == 3) { m->value = -NOS_EINVAL; return; }
        int fd = free_fd(t), desc = free_description();
        if (fd < 0 || desc < 0) { m->value = -NOS_EMFILE; return; }
        if (node < 0 && (flags & NOS_O_CREAT)) node = create(path, MODE_FILE);
        if (node < 0) { m->value = node; return; }
        if (nodes[node].mode == MODE_DIR && ((flags & 3) || (flags & NOS_O_TRUNC))) { m->value = -NOS_EISDIR; return; }
        if ((flags & 3) && backend(path) == 2) { m->value = -NOS_EACCES; return; }
        if ((flags & NOS_O_TRUNC) && (flags & 3)) {
            if (nodes[node].persistent == 2) { rc = nos_fat_write(fat_relative(path), "", 0); if (rc) { m->value = rc; return; } }
            nodes[node].size = 0;
        }
        opened[desc] = (struct description){ .node = node, .flags = flags, .refs = 1, .kind = nodes[node].mode == MODE_CHAR ? 1u : 0u };
        nodes[node].refs++; t->fd[fd] = desc + 1; m->value = fd; return;
    }
    if (m->arg < 0 || (uintptr_t)m->arg >= FDS || !t->fd[m->arg]) { m->value = -NOS_EBADF; return; }
    unsigned fd = (unsigned)m->arg, desc = t->fd[fd] - 1;
    struct description *d = &opened[desc];
    switch (m->op) {
    case NOS_VFS_DUP: {
        intptr_t target = m->value;
        if (target < 0) {
            if (m->len >= FDS) { m->value = -NOS_EINVAL; break; }
            target = -NOS_EMFILE;
            for (unsigned i = (unsigned)m->len; i < FDS; i++) if (!t->fd[i]) { target = (intptr_t)i; break; }
        }
        if (target < 0 || (uintptr_t)target >= FDS) { m->value = target < 0 ? target : -NOS_EBADF; break; }
        if ((unsigned)target != fd) { if (t->fd[target]) drop_description(t->fd[target] - 1); t->fd[target] = desc + 1; d->refs++; }
        m->value = target; break;
    }
    case NOS_VFS_GETFLAGS: m->value = d->flags & (3u | NOS_O_APPEND | NOS_O_NONBLOCK); break;
    case NOS_VFS_SETFLAGS:
        if (m->value < 0 || ((uintptr_t)m->value & ~(3u | NOS_O_APPEND | NOS_O_NONBLOCK))) m->value = -NOS_EINVAL;
        else { d->flags = (d->flags & ~(NOS_O_APPEND | NOS_O_NONBLOCK)) | ((unsigned)m->value & (NOS_O_APPEND | NOS_O_NONBLOCK)); m->value = 0; }
        break;
    case NOS_VFS_CLOSE: t->fd[fd] = 0; drop_description(desc); m->value = 0; break;
    case NOS_VFS_READ: case NOS_VFS_WRITE:
        if (m->value < 0 || ((uintptr_t)m->value & ~NOS_IO_DONTWAIT)) { m->value = -NOS_EINVAL; break; }
        if ((m->op == NOS_VFS_READ && (d->flags & 3) == NOS_O_WRONLY) ||
            (m->op == NOS_VFS_WRITE && !(d->flags & 3))) { m->value = -NOS_EBADF; break; }
        if (d->kind == 2 && m->len && !(d->flags & NOS_O_NONBLOCK) && !(m->value & NOS_IO_DONTWAIT)) {
            int rc = pipe_defer(m, desc); if (rc) m->value = rc; break;
        }
        m->value = m->op == NOS_VFS_READ ? read_data(d, m->data, m->len) : write_data(d, m->data, m->len);
        if (m->op == NOS_VFS_READ && m->value >= 0) m->len = (size_t)m->value;
        break;
    case NOS_VFS_SEEK: {
        if (d->kind) { m->value = -29; break; }
        if (m->len > 2) { m->value = -NOS_EINVAL; break; }
        struct vnode *n = &nodes[d->node];
        intptr_t base = m->len == 0 ? 0 : m->len == 1 ? (intptr_t)d->offset : (intptr_t)n->size;
        if (m->value < -base || m->value > (intptr_t)FILE_BYTES - base) { m->value = -NOS_EINVAL; break; }
        d->offset = (unsigned)(base + m->value); m->value = d->offset; break;
    }
    case NOS_VFS_STAT: {
        struct nos_stat st;
        u_memset(&st, 0, sizeof(st));
        st.size = d->kind ? 0 : nodes[d->node].size;
        st.mode = d->kind == 2 ? MODE_FIFO : d->kind == 1 ? MODE_CHAR : nodes[d->node].mode;
        u_memcpy(m->data, &st, sizeof(st)); m->len = sizeof(st); m->value = 0; break;
    }
    case NOS_VFS_READDIR: {
        if (d->kind || nodes[d->node].mode != MODE_DIR) { m->value = -NOS_ENOTDIR; break; }
        if (m->value < 0) { m->value = -NOS_EINVAL; break; }
        intptr_t index = m->value; const char *dir = nodes[d->node].name; size_t len = u_strlen(dir); unsigned skipped = 0; m->value = 0;
        for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].used && nodes[i].linked && child_path(nodes[i].name, dir)) {
            const char *name = nodes[i].name + len + (len > 1); int deeper = 0;
            for (size_t j = 0; name[j]; j++) if (name[j] == '/') deeper = 1;
            if (deeper || !*name || skipped++ != (uintptr_t)index) continue;
            m->len = u_strlen(name) + 1; u_memcpy(m->data, name, m->len); m->value = 1; break;
        }
        break;
    }
    default: m->value = -NOS_ENOSYS; break;
    }
}
void nos_vfs_server(uintptr_t unused)
{
    (void)unused;
    const char *initial[] = { "/", "/tmp", "/dev", "/proc", "/dev/console", "/proc/version" };
    for (unsigned i = 0; i < 6; i++) {
        u_memcpy(nodes[i].name, initial[i], u_strlen(initial[i]) + 1);
        nodes[i].used = nodes[i].linked = 1; nodes[i].mode = i < 4 ? MODE_DIR : i == 4 ? MODE_CHAR : MODE_FILE;
    }
    const char *version = "NOS microkernel\n";
    nodes[5].size = u_strlen(version); u_memcpy(nodes[5].bytes, version, nodes[5].size);
    for (unsigned i = 0; i < 3; i++) opened[i] = (struct description){ .node = 4, .kind = 1, .flags = i ? NOS_O_WRONLY : NOS_O_RDONLY };
    for (unsigned i = 0; i < NOS_TASKS; i++) fds[i].pid = -1;
    nos_uart_init();
    for (;;) {
        struct nos_message message;
        if (nos_syscall(NOS_SYS_RECV, 0, (uintptr_t)&message, 0)) nos_syscall(NOS_SYS_EXIT, 1, 0, 0);
        uintptr_t sender = (uintptr_t)message.sender;
        if (message.op == NOS_IPC_PEER_CANCEL) { pipe_cancel(message.sender, message.token, 1); pipe_complete(); continue; }
        if (message.op == NOS_IPC_PEER_EXIT) {
            pipe_cancel(message.sender, 0, 0);
            if (!message.value) for (unsigned i = 0; i < NOS_TASKS; i++)
                if (fds[i].pid == (intptr_t)message.process) release_table(&fds[i]);
            pipe_complete(); continue;
        }
        defer_reply = 0; service(&message);
        if (!defer_reply) nos_syscall(NOS_SYS_REPLY, sender, (uintptr_t)&message, 0);
        pipe_complete();
    }
}
