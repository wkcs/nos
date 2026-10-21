/* Copyright (C) 2026 NOS contributors. Private POSIX vnode operations. */
#ifndef NOS_VFS_METADATA_H
#define NOS_VFS_METADATA_H
/* Walk intermediate links before applying a later '..' component. */
static int canonical_path_from(const char *input, size_t bytes, char path[NOS_PATH_MAX], size_t base)
{
    if (!bytes || bytes > JOINED_PATH_MAX || input[0] != '/' || input[bytes - 1] || u_strlen(input) + 1 != bytes) return -NOS_EINVAL;
    PATH_SCOPE;
    PATH_BUFFER(pending, JOINED_PATH_MAX); if (PATH_MISSING(pending)) return -NOS_ENOMEM;
    u_memcpy(pending, input, bytes);
    size_t at = base, end = base; unsigned links = 0; u_memcpy(path, input, base); path[base] = 0;
    for (;;) {
        while (pending[at] == '/') at++;
        if (!pending[at]) break;
        int search = permission_search(path); if (search) return search;
        size_t start = at; while (pending[at] && pending[at] != '/') at++;
        size_t length = at - start, next = at; while (pending[next] == '/') next++;
#ifdef NOS_LINUX_COMPAT
        if (length > NOS_NAME_MAX) return -NOS_ENAMETOOLONG;
#endif
        if (length == 1 && pending[start] == '.') continue;
        if (length == 2 && pending[start] == '.' && pending[start + 1] == '.') {
            if (end > 1) { while (end > 1 && path[end - 1] != '/') end--; if (end > 1) end--; }
            path[end] = 0; continue;
        }
        size_t parent = end;
        if (end > 1) path[end++] = '/';
        if (end + length >= NOS_PATH_MAX) return -NOS_ENAMETOOLONG;
        u_memcpy(path + end, pending + start, length); end += length; path[end] = 0;
        if (!pending[next] && !pending[at]) break;
        int node = lookup(path);
        if (node >= 0 && NODE_TYPE(&nodes[node]) == MODE_LINK) {
            if (links++ >= 8) return -40;
            PATH_SCOPE;
            PATH_BUFFER(joined, JOINED_PATH_MAX); if (PATH_MISSING(joined)) return -NOS_ENOMEM;
            struct vnode *link = &nodes[node]; const char *target = node_bytes(link);
            size_t base = 0, suffix = u_strlen(pending + at);
            if (!link->size) return -NOS_ENOENT;
            if (target[0] != '/') { base = parent; u_memcpy(joined, path, base); if (base > 1) joined[base++] = '/'; }
            if (base + link->size + suffix >= JOINED_PATH_MAX) return -NOS_ENAMETOOLONG;
            u_memcpy(joined + base, target, link->size); u_memcpy(joined + base + link->size, pending + at, suffix + 1);
            u_memcpy(pending, joined, base + link->size + suffix + 1); at = target[0] == '/' ? 1 : parent; end = at; path[end] = 0; continue;
        }
        if (node >= 0) { if (NODE_TYPE(&nodes[node]) != MODE_DIR) return -NOS_ENOTDIR; }
        else { unsigned kind; uint64_t size; int rc = archive_lookup(path, &kind, &size); if (rc < 0) return rc; if (kind != ARCHIVE_DIRECTORY) return -NOS_ENOTDIR; }
    }
    if (input[bytes - 2] == '/' || bytes == 2) { int search = permission_search(path); if (search) return search; }
    int mounted = permission_mount(path); if (mounted) return mounted;
    if (!links && backend(input) != backend(path)) return -NOS_EACCES;
    if (fat_mounted && child_path(path, fat_path))
        for (size_t i = u_strlen(fat_path); path[i]; i++) if (path[i] >= 'a' && path[i] <= 'z') path[i] -= 32;
    return 0;
}

static int canonical_path(const char *input, size_t bytes, char path[NOS_PATH_MAX])
{ return canonical_path_from(input, bytes, path, 1); }

static int inherit_files(intptr_t parent, intptr_t child)
{
    if (parent == child) return 0;
    struct fd_table *source = table_for(parent), *target = table_for(child);
    if (!source || !target) return -NOS_ENOSPC;
#ifdef NOS_LINUX_COMPAT
    int held = directory_ref_hold(&source->cwd); if (held) return held;
#endif
    release_table(target);
    *target = *source; target->pid = child;
    for (unsigned fd = 0; fd < FDS; fd++) if (target->fd[fd]) opened[target->fd[fd] - 1].refs++;
    return 0;
}
static void exec_files(intptr_t group)
{
    for (unsigned i = 0; i < FD_TABLE_COUNT; i++) if (fds[i].pid == group) {
        struct fd_table *table = &fds[i];
        for (unsigned fd = 0; fd < FDS; fd++) if ((table->fd_flags & (1u << fd)) && table->fd[fd]) {
            close_descriptor(table, fd);
        }
        table->fd_flags = 0; return;
    }
}
/* Resolve each symbolic link in the server, including relative link targets.
 * The bounded walk returns ELOOP rather than silently accepting a cycle. */
static int follow_links(char path[NOS_PATH_MAX], int final)
{
    for (unsigned followed = 0;;) {
        unsigned changed = 0;
        for (size_t end = 1;; end++) if (!path[end] || path[end] == '/') {
            PATH_SCOPE;
            PATH_BUFFER(component, NOS_PATH_MAX); if (PATH_MISSING(component)) return -NOS_ENOMEM;
            unsigned last = !path[end];
            u_memcpy(component, path, end); component[end] = 0;
            int node = lookup(component);
            if (node >= 0 && NODE_TYPE(&nodes[node]) == MODE_LINK && (!last || final)) {
                if (followed++ >= 8) return -40;
                PATH_BUFFER(joined, JOINED_PATH_MAX); PATH_BUFFER(resolved, NOS_PATH_MAX);
                if (PATH_MISSING(joined) || PATH_MISSING(resolved)) return -NOS_ENOMEM;
                struct vnode *link = &nodes[node]; const char *target = node_bytes(link); size_t base = 0;
                if (!link->size || link->size >= NOS_PATH_MAX) return -NOS_ENOENT;
                if (target[0] != '/') { base = end; while (base > 1 && path[base - 1] != '/') base--; }
                size_t suffix = u_strlen(path + end);
                if (base + link->size + suffix >= JOINED_PATH_MAX) return -NOS_ENAMETOOLONG;
                u_memcpy(joined, path, base); u_memcpy(joined + base, target, link->size);
                u_memcpy(joined + base + link->size, path + end, suffix + 1);
                int rc = canonical_path_from(joined, base + link->size + suffix + 1, resolved, target[0] == '/' ? 1 : base > 1 ? base - 1 : 1); if (rc) return rc;
                u_memcpy(path, resolved, u_strlen(resolved) + 1); changed = 1; break;
            }
            if (!last) {
                if (node >= 0 && NODE_TYPE(&nodes[node]) != MODE_DIR) return -NOS_ENOTDIR;
                if (node < 0) { unsigned kind; uint64_t size; int rc = archive_lookup(component, &kind, &size);
                    if (rc < 0) return rc;
                    if (kind != ARCHIVE_DIRECTORY) return -NOS_ENOTDIR;
                }
            }
            if (last) break;
        }
        if (!changed) return 0;
    }
}
static int metadata_path(struct fd_table *table, intptr_t dirfd, const char *input, size_t len, char path[NOS_PATH_MAX], int final)
{
    int rc = resolve_path(table, dirfd, input, len, path);
    return rc ? rc : follow_links(path, final);
}
static int metadata_node(const char *path)
{
    int node = lookup(path);
    if (node >= 0) return node;
    unsigned kind; uint64_t size;
    return archive_lookup(path, &kind, &size) >= 0 ? -30 : node;
}
static int resize_node(unsigned index, uint64_t length)
{
    struct vnode *node = &nodes[index];
    if (NODE_TYPE(node) == MODE_DIR) return -NOS_EISDIR;
    if (NODE_TYPE(node) != MODE_FILE) return -NOS_EINVAL;
    if (backend(node->name)) return -NOS_EACCES;
    if (length > SIZE_MAX) return -27;
#ifdef NOS_LINUX_COMPAT
    int64_t now; int clock = permission_clock(&now); if (clock) return clock;
#endif
    int reserve = node_reserve(node, (size_t)length); if (reserve) return reserve;
    if (node->persistent == 2 && node->linked) {
        char data[INLINE_BYTES]; size_t retained = node->size < length ? node->size : (size_t)length;
        u_memset(data, 0, sizeof(data)); u_memcpy(data, node_bytes(node), retained);
        int rc = nos_fat_write(fat_relative(node->name), data, (size_t)length); if (rc) return rc;
    }
    if (length < node->size) u_memset(node_bytes(node) + (size_t)length, 0, node->size - (size_t)length);
    else if (length > node->size) u_memset(node_bytes(node) + node->size, 0, (size_t)length - node->size);
    node->size = (size_t)length;
#ifdef NOS_LINUX_COMPAT
    permission_touch(index, 6, now); if (subject.credentials.euid) node->mode &= ~06000u;
#endif
    return 0;
}
static int change_mode(unsigned index, unsigned mode)
{
#ifdef NOS_LINUX_COMPAT
    if (nodes[index].persistent) return -NOS_ENOTSUP;
#else
    if (nodes[index].persistent == 2) return -NOS_ENOTSUP;
#endif
    if (backend(nodes[index].name)) return -NOS_EACCES;
#ifdef NOS_LINUX_COMPAT
    int rc = permission_owner(index); if (rc) return rc;
    if (NODE_TYPE(&nodes[index]) != MODE_DIR && (mode & 06000u)) return -NOS_ENOTSUP;
    if (subject.credentials.euid && (mode & 02000u) && !permission_group(security[index].gid)) mode &= ~02000u;
    int64_t now; rc = permission_clock(&now); if (rc) return rc;
    permission_touch(index, 4, now);
#endif
    nodes[index].mode = NODE_TYPE(&nodes[index]) | (mode & 07777u); return 0;
}
static int split_paths(const struct path_payload *payload, size_t *first)
{
    size_t at = 0; while (at < payload->len && payload->bytes[at]) at++;
    if (at == payload->len || at + 1 == payload->len || payload->bytes[payload->len - 1]) return -NOS_EINVAL;
    *first = at + 1;
    if (*first > NOS_PATH_MAX || payload->len - *first > NOS_PATH_MAX) return -NOS_ENAMETOOLONG;
    return 0;
}
#ifdef NOS_LINUX_COMPAT
static int credential_service(struct nos_message *m, struct fd_table *table, struct path_payload *payload)
{
    PATH_SCOPE;
    if (m->op < NOS_VFS_ACCESS || m->op > NOS_VFS_FCHOWN) return 0;
    if (m->op == NOS_VFS_ACCESS) {
        uintptr_t value = (uintptr_t)m->value;
        unsigned mode = value & 7u, flags = value >> 8;
        if (value & ~(7u | ((uintptr_t)0x300u << 8))) { m->value = -NOS_EINVAL; return 1; }
        permission_real_ids = !(flags & 0x200u);
        PATH_BUFFER(path, NOS_PATH_MAX); if (PATH_MISSING(path)) { m->value = -NOS_ENOMEM; return 1; }
        int rc = metadata_path(table, m->arg, payload->bytes, payload->len, path, !(flags & 0x100u));
        if (!rc) rc = permission_path(path, mode);
        permission_real_ids = 0; m->value = rc; return 1;
    }
    uint32_t ids[2];
    if (m->len != sizeof(ids)) { m->value = -NOS_EINVAL; return 1; }
    u_memcpy(ids, m->data, sizeof(ids));
    if (m->op == NOS_VFS_FCHOWN) {
        if (m->value) { m->value = -NOS_EINVAL; return 1; }
        if (m->arg < 0 || (uintptr_t)m->arg >= FDS || !table->fd[m->arg]) { m->value = -NOS_EBADF; return 1; }
        struct description *d = &opened[table->fd[m->arg] - 1];
        m->value = d->kind >= ARCHIVE_FILE ? -30 : d->kind ? -NOS_ENOTSUP : permission_chown(d->node, ids[0], ids[1]);
        return 1;
    }
    if (!m->bulk_in_len || m->value < 0 || ((uintptr_t)m->value & ~0x100u)) { m->value = -NOS_EINVAL; return 1; }
    PATH_BUFFER(path, NOS_PATH_MAX); if (PATH_MISSING(path)) { m->value = -NOS_ENOMEM; return 1; }
    int rc = metadata_path(table, m->arg, payload->bytes, payload->len, path, !(m->value & 0x100u));
    int node = rc ? rc : metadata_node(path);
    m->value = node < 0 ? node : permission_chown((unsigned)node, ids[0], ids[1]);
    return 1;
}
#endif
static int metadata_service(struct nos_message *m, struct fd_table *table, struct path_payload *payload)
{
    PATH_SCOPE;
    unsigned op = m->op;
    if (op < NOS_VFS_FTRUNCATE || op > 43 || op == NOS_VFS_OPENAT_MODE) return 0;
    if (op == NOS_VFS_UMASK) { unsigned old = table->mask; table->mask = (unsigned)m->value & 0777u; m->value = old; return 1; }
    if (op == NOS_VFS_FTRUNCATE || op == NOS_VFS_FCHMOD) {
        if (m->arg < 0 || (uintptr_t)m->arg >= FDS || !table->fd[m->arg]) { m->value = -NOS_EBADF; return 1; }
        struct description *d = &opened[table->fd[m->arg] - 1];
        if (d->kind >= ARCHIVE_FILE) { m->value = -30; return 1; }
        if (d->kind) { m->value = -NOS_EINVAL; return 1; }
        if (op == NOS_VFS_FCHMOD) m->value = change_mode(d->node, (unsigned)m->value);
        else if (!(d->flags & 3)) m->value = -NOS_EBADF;
        else if (m->len != 8) m->value = -NOS_EINVAL;
        else { uint64_t length; u_memcpy(&length, m->data, 8); m->value = length > INT64_MAX ? -NOS_EINVAL : resize_node(d->node, length); }
        return 1;
    }
    PATH_BUFFER(path, NOS_PATH_MAX); if (PATH_MISSING(path)) { m->value = -NOS_ENOMEM; return 1; }
    int rc;
    if (op == NOS_VFS_SYMLINK || op == NOS_VFS_LINK || op == 43) {
        size_t first; rc = split_paths(payload, &first);
        if (rc) { m->value = rc; return 1; }
        PATH_BUFFER(dest, NOS_PATH_MAX); if (PATH_MISSING(dest)) { m->value = -NOS_ENOMEM; return 1; }
        rc = metadata_path(table, op == NOS_VFS_SYMLINK ? m->arg : m->value, payload->bytes + first, payload->len - first, dest, 0);
        if (rc) { m->value = rc; return 1; }
        if (op == NOS_VFS_SYMLINK) {
            if (first == 1) { m->value = -NOS_ENOENT; return 1; }
            if (metadata_node(dest) != -NOS_ENOENT) { m->value = -NOS_EEXIST; return 1; }
            int parent = parent_of(dest);
            if (parent < 0) { m->value = parent; return 1; }
            if (nodes[parent].persistent) { m->value = -NOS_ENOTSUP; return 1; }
            int node = create_sized(dest, MODE_LINK | 0777, first);
            if (node >= 0) { nodes[node].size = first - 1; u_memcpy(node_bytes(&nodes[node]), payload->bytes, first); }
            m->value = node < 0 ? node : 0; return 1;
        }
        rc = metadata_path(table, m->arg, payload->bytes, first, path, 0);
        if (rc) { m->value = rc; return 1; }
        if (op == 43) {
            size_t a = u_strlen(path) + 1, b = u_strlen(dest) + 1;
            if (a + b > PATH_DATA_MAX) { m->value = -NOS_ENAMETOOLONG; return 1; }
            u_memcpy(payload->bytes, path, a); u_memcpy(payload->bytes + a, dest, b); payload->len = a + b; payload->resolved = 1; m->op = NOS_VFS_RENAME; return 0;
        }
        int node = metadata_node(path);
        if (node < 0) { m->value = node; return 1; }
        if (NODE_TYPE(&nodes[node]) == MODE_DIR) { m->value = -NOS_EPERM; return 1; }
        if (metadata_node(dest) != -NOS_ENOENT) { m->value = -NOS_EEXIST; return 1; }
        int parent = parent_of(dest);
        if (parent < 0) { m->value = parent; return 1; }
        rc = permission_node((unsigned)parent, 3); if (rc) { m->value = rc; return 1; }
        if (nodes[parent].persistent != nodes[node].persistent) { m->value = -18; return 1; }
        if (nodes[node].persistent || backend(path) || backend(dest)) { m->value = -NOS_ENOTSUP; return 1; }
        for (unsigned i = 0; i < NODE_COUNT; i++) if (!nodes[i].used) {
#ifdef NOS_LINUX_COMPAT
            int64_t now; rc = permission_clock(&now); if (rc) { m->value = rc; return 1; }
            permission_touch((unsigned)parent, 6, now); permission_touch((unsigned)node, 4, now);
#endif
            clear_node(i); nodes[i].used = 2; nodes[i].linked = 1; nodes[i].size = (size_t)node;
            u_memcpy(nodes[i].name, dest, u_strlen(dest) + 1); m->value = 0; return 1;
        }
        m->value = -NOS_ENOSPC; return 1;
    }
    intptr_t dirfd = op == NOS_VFS_CHMOD || op == NOS_VFS_MKDIRAT || op == 42 ? m->arg : AT_CURRENT_DIRECTORY;
    rc = metadata_path(table, dirfd, payload->bytes, payload->len, path, op != 42);
    if (rc) { m->value = rc; return 1; }
    if (op == 42) {
        if (m->value != 0 && m->value != 0x200) { m->value = -NOS_EINVAL; return 1; }
        m->op = m->value ? NOS_VFS_RMDIR : NOS_VFS_UNLINK;
        payload->len = u_strlen(path) + 1; u_memcpy(payload->bytes, path, payload->len); return 0;
    }
    int node = metadata_node(path);
    if (op == NOS_VFS_MKDIRAT) {
        if (node != -NOS_ENOENT) m->value = node >= 0 || node == -30 ? -NOS_EEXIST : node;
        else { node = create(path, MODE_DIR | ((unsigned)m->value & 07777u & ~table->mask)); m->value = node < 0 ? node : 0; }
    } else if (node < 0) m->value = node;
    else if (op == NOS_VFS_CHMOD) m->value = change_mode((unsigned)node, (unsigned)m->value);
    else if (op == NOS_VFS_TRUNCATE) { int permitted = permission_node((unsigned)node, 2); m->value = permitted ? permitted : m->arg < 0 ? -NOS_EINVAL : resize_node((unsigned)node, (uint64_t)m->arg); }
    else m->value = -NOS_ENOTSUP;
    return 1;
}
#endif
