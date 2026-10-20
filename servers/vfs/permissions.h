/* Copyright (C) 2026 NOS contributors. User-space discretionary permissions. */
#ifndef NOS_VFS_PERMISSIONS_H
#define NOS_VFS_PERMISSIONS_H
#ifdef NOS_LINUX_COMPAT
struct inode_security { uint32_t uid, gid; int64_t atime, mtime, ctime; };
static struct inode_security security[NODE_COUNT], pipe_security[PIPE_COUNT];
static struct nos_ipc_subject subject;
static unsigned permission_real_ids;
static int permission_subject(struct nos_message *message)
{
    struct nos_ipc_subject snapshot;
    struct nos_ipc_subject_query query = { .sender = (uint32_t)message->sender,
        .token = message->token, .output = (uintptr_t)&snapshot };
    int rc = (int)nos_syscall(NOS_SYS_IPC_SUBJECT, (uintptr_t)&query, 0, 0);
    if (rc) return rc;
    if (snapshot.token != message->token || snapshot.sender != (uintptr_t)message->sender ||
        snapshot.process != message->process || snapshot.credentials.ngroups > NOS_NGROUPS_MAX ||
        snapshot.credentials.reserved) return -NOS_EPERM;
    subject = snapshot; permission_real_ids = 0; return 0;
}
static uint32_t permission_uid(void)
{ return permission_real_ids ? subject.credentials.ruid : subject.credentials.euid; }
static int permission_group(uint32_t gid)
{
    const struct nos_credentials *c = &subject.credentials;
    if (gid == (permission_real_ids ? c->rgid : c->egid)) return 1;
    for (unsigned i = 0; i < c->ngroups; i++) if (c->groups[i] == gid) return 1;
    return 0;
}
static int permission_bits(unsigned mode, uint32_t uid, uint32_t gid, unsigned access)
{
    if (!permission_uid()) {
        if ((access & 1) && (mode & 0170000u) != MODE_DIR && !(mode & 0111)) return -NOS_EACCES;
        return 0;
    }
    unsigned shift = permission_uid() == uid ? 6 : permission_group(gid) ? 3 : 0;
    return (((mode >> shift) & access) == access) ? 0 : -NOS_EACCES;
}
static int permission_node(unsigned node, unsigned access)
{
    /* Persistent formats have no durable ownership. The Linux profile exposes
     * these mounts only to root; existing authorized OFDs remain usable. */
    if (nodes[node].persistent && permission_uid()) return -NOS_EACCES;
    return permission_bits(nodes[node].mode, security[node].uid, security[node].gid, access);
}
static unsigned permission_archive_mode(const char *path, unsigned kind)
{
    if (kind == 4u) return MODE_DIR | 0555;
    return MODE_FILE | ((child_path(path, "/bin") || child_path(path, "/usr/bin")) ? 0555 : 0444);
}
static int permission_path(const char *path, unsigned access)
{
    int node = lookup(path);
    if (node >= 0) return permission_node((unsigned)node, access);
    unsigned kind; uint64_t size;
    int rc = archive_lookup(path, &kind, &size);
    return rc < 0 ? rc : permission_bits(permission_archive_mode(path, kind), 0, 0, access);
}
static int permission_search(const char *path)
{
    int node = lookup(path);
    if (node >= 0) return NODE_TYPE(&nodes[node]) == MODE_DIR ? permission_node((unsigned)node, 1) : -NOS_ENOTDIR;
    unsigned kind; uint64_t size; int rc = archive_lookup(path, &kind, &size);
    if (rc < 0) return rc;
    return kind == 4u ? permission_bits(MODE_DIR | 0555, 0, 0, 1) : -NOS_ENOTDIR;
}
static int permission_mount(const char *path)
{ int node = lookup(path); return node >= 0 && nodes[node].persistent && permission_uid() ? -NOS_EACCES : 0; }
static int permission_parent(const char *path)
{ int parent = parent_of(path); return parent < 0 ? parent : permission_node((unsigned)parent, 3); }
static int permission_sticky(unsigned parent, unsigned victim)
{
    return !(nodes[parent].mode & 01000) || !subject.credentials.euid ||
        subject.credentials.euid == security[parent].uid || subject.credentials.euid == security[victim].uid ? 0 : -NOS_EPERM;
}
static int permission_clock(int64_t *seconds)
{
    struct nos_clock_value value;
    int rc = (int)nos_syscall(NOS_SYS_CLOCK_GET64, 0, (uintptr_t)&value, 0);
    if (rc) return rc;
    if (value.seconds < 0 || value.nanoseconds < 0 || value.nanoseconds >= 1000000000) return -NOS_EIO;
    *seconds = value.seconds; return 0;
}
static void permission_touch(unsigned node, unsigned what, int64_t now)
{
    if (what & 1) security[node].atime = now;
    if (what & 2) security[node].mtime = now;
    if (what & 4) security[node].ctime = now;
}
static int permission_owner(unsigned node)
{ return !subject.credentials.euid || subject.credentials.euid == security[node].uid ? 0 : -NOS_EPERM; }
static int permission_chown(unsigned node, uint32_t uid, uint32_t gid)
{
    if (nodes[node].persistent) return -NOS_ENOTSUP;
    if (backend(nodes[node].name)) return -NOS_EACCES;
    int rc = permission_owner(node); if (rc) return rc;
    if (subject.credentials.euid && ((uid != UINT32_MAX && uid != security[node].uid) ||
        (gid != UINT32_MAX && !permission_group(gid)))) return -NOS_EPERM;
    int64_t now; rc = permission_clock(&now); if (rc) return rc;
    if (uid != UINT32_MAX) security[node].uid = uid;
    if (gid != UINT32_MAX) security[node].gid = gid;
    if (NODE_TYPE(&nodes[node]) != MODE_DIR) nodes[node].mode &= ~06000u;
    permission_touch(node, 4, now); return 0;
}
#else
#define permission_search(path) ((void)(path), 0)
#define permission_mount(path) ((void)(path), 0)
#define permission_parent(path) ((void)(path), 0)
#define permission_node(node,access) ((void)(node), (void)(access), 0)
#define permission_path(path,access) ((void)(path), (void)(access), 0)
#define permission_sticky(parent,victim) ((void)(parent), (void)(victim), 0)
#endif
#endif
