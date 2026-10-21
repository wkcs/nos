/* Copyright (C) 2026 NOS contributors. Private user-space archive backend. */
#ifndef NOS_VFS_ARCHIVE_H
#define NOS_VFS_ARCHIVE_H
#define ARCHIVE_FILE 3u
#define ARCHIVE_DIRECTORY 4u
#define AT_CURRENT_DIRECTORY (-100)
static int archive_name(unsigned index, char name[VFS_ARCHIVE_PATH_MAX])
{
    struct nos_bootfile_io io = { .index = index, .buffer = (uintptr_t)name, .count = VFS_ARCHIVE_PATH_MAX };
    intptr_t result = nos_syscall(NOS_SYS_BOOTFILE, NOS_BOOTFILE_ENUM, (uintptr_t)&io, 0);
    return result < 0 ? (int)result : 0;
}
static int archive_stat(unsigned index, struct nos_stat64 *st)
{
    struct nos_bootfile_io io = { .index = index, .buffer = (uintptr_t)st, .count = sizeof(*st) };
    intptr_t result = nos_syscall(NOS_SYS_BOOTFILE, NOS_BOOTFILE_STAT, (uintptr_t)&io, 0);
    if (result < 0) return (int)result;
#ifdef NOS_LINUX_COMPAT
    char path[VFS_ARCHIVE_PATH_MAX]; int rc = archive_name(index, path); if (rc) return rc;
    st->mode = permission_archive_mode(path, ARCHIVE_FILE);
#endif
    return 0;
}
static intptr_t archive_read(unsigned index, uint64_t offset, void *data, size_t count)
{
    struct nos_bootfile_io io = { .index = index, .offset = offset, .buffer = (uintptr_t)data, .count = count };
    return nos_syscall(NOS_SYS_BOOTFILE, NOS_BOOTFILE_READ, (uintptr_t)&io, 0);
}
static int archive_lookup(const char *path, unsigned *kind, uint64_t *size)
{
    if (u_strlen(path) >= VFS_ARCHIVE_PATH_MAX) return -NOS_ENOENT;
    struct nos_bootfile_io io = { .buffer = (uintptr_t)path, .count = u_strlen(path) + 1 };
    intptr_t index = nos_syscall(NOS_SYS_BOOTFILE, NOS_BOOTFILE_LOOKUP, (uintptr_t)&io, 0);
    if (index >= 0) {
        struct nos_stat64 st; int rc = archive_stat((unsigned)index, &st); if (rc) return rc;
        *kind = ARCHIVE_FILE; *size = st.size; return (int)index;
    }
    if (index != -NOS_ENOENT && index != -NOS_ENOSYS && index != -NOS_ENOTSUP) return (int)index;
    for (unsigned i = 0;; i++) {
        char name[VFS_ARCHIVE_PATH_MAX]; if (archive_name(i, name)) break;
        if (child_path(name, path)) { *kind = ARCHIVE_DIRECTORY; *size = u_strlen(path); return (int)i; }
    }
    return -NOS_ENOENT;
}
static int description_path(const struct description *d, char *path)
{
    if (d->kind == 2) return -NOS_ENOTDIR;
    if (d->kind >= ARCHIVE_FILE) {
        int rc = archive_name(d->node, path); if (rc) return rc;
        if (d->kind == ARCHIVE_DIRECTORY) { if (d->size >= VFS_ARCHIVE_PATH_MAX) return -NOS_EIO; path[d->size] = 0; }
    } else {
#ifdef NOS_LINUX_COMPAT
        if (d->node >= NODE_COUNT || !nodes[d->node].used || !nodes[d->node].linked) return -NOS_ENOENT;
#endif
        u_memcpy(path, nodes[d->node].name, u_strlen(nodes[d->node].name) + 1);
    }
    return 0;
}
static int description_directory(const struct description *d)
{ return d->kind == ARCHIVE_DIRECTORY || (!d->kind && NODE_TYPE(&nodes[d->node]) == MODE_DIR); }
#ifdef NOS_LINUX_COMPAT
static int cwd_path(const struct fd_table *table, char path[NOS_PATH_MAX])
{
    struct description directory = { .node = table->cwd.node, .kind = table->cwd.kind, .size = table->cwd.size };
    return description_path(&directory, path);
}
static int cwd_set_path(struct fd_table *table, const char *path)
{
    int node = lookup(path);
    if (node >= 0) return directory_ref_set(table, (struct directory_ref){ .node = (uint16_t)node });
    unsigned kind; uint64_t size; int index = archive_lookup(path, &kind, &size);
    if (index < 0) return index;
    if (kind != ARCHIVE_DIRECTORY || size > UINT32_MAX || (unsigned)index >= NO_NODE) return -NOS_ENOTDIR;
    return directory_ref_set(table, (struct directory_ref){ .node = (uint16_t)index, .kind = (uint16_t)kind, .size = (uint32_t)size });
}
static int cwd_set_description(struct fd_table *table, const struct description *directory)
{
    if (!description_directory(directory)) return -NOS_ENOTDIR;
    int rc = directory->kind == ARCHIVE_DIRECTORY ? 0 : permission_node(directory->node, 1);
    if (rc) return rc;
    if (directory->size > UINT32_MAX) return -NOS_ENOTDIR;
    return directory_ref_set(table, (struct directory_ref){ .node = directory->node, .kind = directory->kind, .size = (uint32_t)directory->size });
}
#endif
static int resolve_path(struct fd_table *table, intptr_t dirfd, const char *input, size_t bytes, char path[NOS_PATH_MAX])
{
    if (!bytes || bytes > NOS_PATH_MAX || input[bytes - 1] || u_strlen(input) + 1 != bytes) return -NOS_EINVAL;
    if (input[0] == '/') return canonical_path(input, bytes, path);
    if (bytes == 1) return -NOS_ENOENT;
    PATH_SCOPE;
    PATH_BUFFER(base, NOS_PATH_MAX); PATH_BUFFER(joined, JOINED_PATH_MAX);
    if (PATH_MISSING(base) || PATH_MISSING(joined)) return -NOS_ENOMEM;
    if (dirfd == AT_CURRENT_DIRECTORY) {
#ifdef NOS_LINUX_COMPAT
        int rc = cwd_path(table, base); if (rc) return rc;
#else
        u_memcpy(base, table->cwd, u_strlen(table->cwd) + 1);
#endif
    }
    else {
        if (dirfd < 0 || (uintptr_t)dirfd >= FDS || !table->fd[dirfd]) return -NOS_EBADF;
        struct description *d = &opened[table->fd[dirfd] - 1];
        if (!description_directory(d)) return -NOS_ENOTDIR;
        int rc = description_path(d, base); if (rc) return rc;
    }
    size_t n = u_strlen(base), base_bytes = n, slash = n > 1;
    if (n + slash + bytes > JOINED_PATH_MAX) return -NOS_ENAMETOOLONG;
    u_memcpy(joined, base, n); if (slash) joined[n++] = '/';
    u_memcpy(joined + n, input, bytes); return canonical_path_from(joined, n + bytes, path, base_bytes);
}
static uint64_t path_inode(const char *path)
{
    unsigned rank = 0;
    for (unsigned i = 0;; i++) {
        char file[VFS_ARCHIVE_PATH_MAX]; if (archive_name(i, file)) break;
        for (size_t at = 1; file[at]; at++) if (file[at] == '/') {
            char candidate[VFS_ARCHIVE_PATH_MAX]; u_memcpy(candidate, file, at); candidate[at] = 0;
            if (lookup(candidate) >= 0) continue;
            unsigned seen = 0;
            for (unsigned previous = 0; previous < i; previous++) {
                char old[VFS_ARCHIVE_PATH_MAX]; if (archive_name(previous, old)) break;
                if (u_streq(old, candidate) || child_path(old, candidate)) { seen = 1; break; }
            }
            if (seen) continue;
            if (rank >= 0xc000u) return UINT64_MAX;
            if (u_streq(candidate, path)) return 0x4000u + rank;
            rank++;
        }
    }
    return UINT64_MAX;
}
static int full_stat(const struct description *d, struct nos_stat64 *st)
{
    u_memset(st, 0, sizeof(*st));
    if (d->kind == ARCHIVE_FILE) return archive_stat(d->node, st);
    if (d->kind == ARCHIVE_DIRECTORY) {
        char path[VFS_ARCHIVE_PATH_MAX]; int rc = description_path(d, path); if (rc) return rc;
        st->ino = path_inode(path); if (st->ino == UINT64_MAX) return -NOS_EOVERFLOW; st->mode = MODE_DIR | 0555; st->nlink = 2; return 0;
    }
    st->ino = d->kind == 2 ? 0x2000u + (unsigned)d->pipe : node_inode(d->node);
    st->size = d->kind ? 0 : nodes[d->node].size;
    st->blocks = (st->size + 511) / 512;
    st->mode = d->kind == 2 ? MODE_FIFO | 0600 : d->kind == 1 ? MODE_CHAR | 0666 : nodes[d->node].mode;
    st->nlink = !d->kind ? node_links(d->node) : 1;
#ifdef NOS_LINUX_COMPAT
    const struct inode_security *metadata = d->kind == 2 ? &pipe_security[d->pipe] : &security[d->node];
    st->uid = metadata->uid; st->gid = metadata->gid;
    st->atime_sec = metadata->atime; st->mtime_sec = metadata->mtime; st->ctime_sec = metadata->ctime;
#endif
    return 0;
}
/* Directory cookies index the union of RAM nodes and unique archive children.
 * A continuation reconstructs one immutable archive name without moving fdpos. */
static int directory_entry(const struct description *directory, uint64_t cookie, char name[VFS_DIRENT_NAME_BYTES], struct nos_stat64 *st)
{
    PATH_SCOPE;
    PATH_BUFFER(path, NOS_PATH_MAX); if (PATH_MISSING(path)) return -NOS_ENOMEM;
    int rc = description_path(directory, path); if (rc) return rc;
    if (!description_directory(directory)) return -NOS_ENOTDIR;
    if (cookie < 2) {
        const char *dot = cookie ? ".." : "."; u_memcpy(name, dot, cookie ? 3 : 2);
        if (!cookie) return full_stat(directory, st) ? -NOS_EIO : 1;
        size_t end = u_strlen(path); while (end > 1 && path[end - 1] != '/') end--;
        if (end > 1) end--;
        path[end] = 0;
        int node = lookup(path); struct description parent = { 0 };
        if (node >= 0) parent.node = (unsigned)node;
        else { unsigned kind; uint64_t size; int index = archive_lookup(path, &kind, &size); if (index < 0) return index;
            parent.node = (unsigned)index; parent.kind = kind; parent.size = size; }
        return full_stat(&parent, st) ? -NOS_EIO : 1;
    }
    uint64_t found = 2; size_t parent_bytes = u_strlen(path);
    for (unsigned i = 0; i < NODE_COUNT; i++) if (nodes[i].used && nodes[i].linked && child_path(nodes[i].name, path)) {
        const char *child = nodes[i].name + parent_bytes + (parent_bytes > 1); unsigned nested = 0;
        for (size_t j = 0; child[j]; j++) if (child[j] == '/') nested = 1;
        if (!*child || nested) continue;
        if (found++ == cookie) { u_memcpy(name, child, u_strlen(child) + 1); struct description d = { .node = inode_of(i) }; return full_stat(&d, st) ? -NOS_EIO : 1; }
    }
    for (unsigned index = 0;; index++) {
        char file[VFS_ARCHIVE_PATH_MAX]; if (archive_name(index, file)) break;
        if (!child_path(file, path)) continue;
        const char *child = file + parent_bytes + (parent_bytes > 1); size_t length = 0;
        while (child[length] && child[length] != '/') length++;
        if (!length) continue;
        PATH_SCOPE;
        PATH_BUFFER(full, NOS_PATH_MAX); if (PATH_MISSING(full)) return -NOS_ENOMEM;
        size_t at = parent_bytes; u_memcpy(full, path, at);
        if (at > 1) full[at++] = '/';
        u_memcpy(full + at, child, length); full[at + length] = 0;
        if (lookup(full) >= 0) continue;
        unsigned duplicate = 0;
        for (unsigned previous = 0; previous < index; previous++) {
            char old[VFS_ARCHIVE_PATH_MAX]; if (archive_name(previous, old)) break;
            if (u_streq(old, full) || child_path(old, full)) { duplicate = 1; break; }
        }
        if (duplicate) continue;
        if (found++ != cookie) continue;
        u_memcpy(name, child, length); name[length] = 0;
        unsigned kind; uint64_t size; int id = archive_lookup(full, &kind, &size); if (id < 0) return id;
        struct description d = { .node = (unsigned)id, .kind = kind, .size = size };
        rc = full_stat(&d, st); return rc ? rc : 1;
    }
    return 0;
}
static void get_directory(struct nos_message *m, struct description *d)
{
    if (!description_directory(d)) { m->value = -NOS_ENOTDIR; return; }
    if (m->value < 0 || !m->len || m->len > NOS_MSG_DATA) { m->value = -NOS_EINVAL; return; }
    uint64_t cookie = d->offset, capacity; u_memcpy(&capacity, m->data + 8, 8);
    if (m->value) u_memcpy(&cookie, m->data, 8);
    char name[VFS_DIRENT_NAME_BYTES]; struct nos_stat64 st; int rc = directory_entry(d, cookie, name, &st);
    if (rc <= 0) { m->value = rc; m->len = 0; return; }
    unsigned char record[VFS_DIRENT_NAME_BYTES + 24]; u_memset(record, 0, sizeof(record));
    uint16_t bytes = (uint16_t)((19 + u_strlen(name) + 1 + 7) & ~(size_t)7);
    if (bytes > sizeof(record) || (uint64_t)m->value >= bytes || (!m->value && capacity < bytes)) { m->value = -NOS_EINVAL; return; }
    uint64_t next = cookie + 1; u_memcpy(record, &st.ino, 8); u_memcpy(record + 8, &next, 8); u_memcpy(record + 16, &bytes, 2);
    record[18] = (unsigned char)((st.mode >> 12) & 15); u_memcpy(record + 19, name, u_strlen(name) + 1);
    size_t fragment = (size_t)m->value, count = bytes - fragment; if (count > m->len) count = m->len;
    if (!fragment) d->offset = next;
    u_memcpy(m->data, record + fragment, count); m->arg = (intptr_t)cookie; m->value = bytes; m->len = count;
}
#endif
