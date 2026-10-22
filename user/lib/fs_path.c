/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
#include "user.h"

/* Bounded pathname snapshots and namespace operations. */

/* Path snapshot transport. */
#ifdef NOS_LINUX_COMPAT
/* Keep the common short pathname on the stack; long snapshots have private,
 * operation-scoped mappings rather than PATH_MAX-sized gateway frames. */
struct path_snapshot {
    char initial[NOS_MSG_DATA];
    char *data;
    size_t length;
    uintptr_t allocation;
};
static void path_snapshot_release(struct path_snapshot *snapshot)
{
    if (snapshot->allocation)
        (void)nos_syscall(NOS_SYS_MUNMAP, snapshot->allocation, NOS_PATH_MAX, 0);
}
/* A chunk never crosses a source page, so a NUL immediately before a guard
 * is accepted without touching the guard. No call reads byte PATH_MAX + 1. */
static int copy_path_part(char *destination, uintptr_t source, size_t start,
                          size_t capacity, size_t *length)
{
    for (size_t at = start; at < capacity;) {
        if (at > UINTPTR_MAX - source)
            return -NOS_EFAULT;
        uintptr_t address = source + at;
        size_t chunk = NOS_PAGE_SIZE - (address & (NOS_PAGE_SIZE - 1u));
        if (chunk > 256u)
            chunk = 256u;
        if (chunk > capacity - at)
            chunk = capacity - at;
        int rc = nos_user_copy(destination + at, (const void *)address, chunk);
        if (rc)
            return rc;
        for (size_t i = 0; i < chunk; i++)
            if (!destination[at + i]) {
                *length = at + i + 1;
                return 0;
            }
        at += chunk;
    }
    return -NOS_ENAMETOOLONG;
}

static int path_snapshot_copy(struct path_snapshot *snapshot, const char *path)
{
    if (!path)
        return -NOS_EFAULT;
    snapshot->data = snapshot->initial;
    int rc = copy_path_part(snapshot->initial, (uintptr_t)path, 0,
                            sizeof(snapshot->initial), &snapshot->length);
    if (rc != -NOS_ENAMETOOLONG)
        return rc;
    intptr_t allocation = nos_syscall(NOS_SYS_MMAP, NOS_PROT_READ | NOS_PROT_WRITE, NOS_PATH_MAX, 0);
    if (nos_pointer_is_error(allocation))
        return (int)allocation;
    snapshot->allocation = (uintptr_t)allocation;
    snapshot->data = (char *)(uintptr_t)allocation;
    u_memcpy(snapshot->data, snapshot->initial, sizeof(snapshot->initial));
    return copy_path_part(snapshot->data, (uintptr_t)path, sizeof(snapshot->initial),
                          NOS_PATH_MAX, &snapshot->length);
}
#define PATH_SNAPSHOT(name) struct path_snapshot name __attribute__((cleanup(path_snapshot_release))) = {0}
static int path_output_prepare(struct path_snapshot *snapshot, size_t capacity)
{
    snapshot->data = snapshot->initial;
    if (capacity <= sizeof(snapshot->initial))
        return 0;
    intptr_t allocation = nos_syscall(NOS_SYS_MMAP, NOS_PROT_READ | NOS_PROT_WRITE, NOS_PATH_MAX, 0);
    if (nos_pointer_is_error(allocation))
        return (int)allocation;
    snapshot->allocation = (uintptr_t)allocation;
    snapshot->data = (char *)(uintptr_t)allocation;
    return 0;
}
#else
static int copy_path(char *destination, const char *path, size_t *length)
{
    if (!path)
        return -NOS_EFAULT;
    for (size_t i = 0; i < NOS_PATH_MAX; i++) {
        int rc = nos_user_copy(destination + i, path + i, 1);
        if (rc)
            return rc;
        if (!destination[i]) {
            *length = i + 1;
            return 0;
        }
    }
    return -NOS_ENAMETOOLONG;
}
#endif
static intptr_t path_call(struct nos_message *message, const char *path)
{
#ifdef NOS_LINUX_COMPAT
    PATH_SNAPSHOT(snapshot);
    int rc = path_snapshot_copy(&snapshot, path);
    if (rc)
        return rc;
    size_t bytes = snapshot.length;
    const char *safe_path = snapshot.data;
#else
    char safe_path[NOS_PATH_MAX];
    size_t bytes;
    int rc = copy_path(safe_path, path, &bytes);
    if (rc)
        return rc;
#endif
#ifdef NOS_LINUX_COMPAT
    if (bytes > NOS_MSG_DATA) {
        message->bulk_in = (uintptr_t)safe_path;
        message->bulk_in_len = bytes;
        message->len = 0;
    } else
#endif
    {
        u_memcpy(message->data, safe_path, bytes);
        message->len = bytes;
    }
    return nos_rpc(message);
}

static int path_request(unsigned op, const char *path, unsigned flags)
{
    struct nos_message m = { .op = op, .arg = flags };
    return (int)path_call(&m, path);
}

/* Open, create, and pathname metadata. */
int nos_openat(int directory, const char *path, unsigned flags)
{
    struct nos_message m = { .op = NOS_VFS_OPENAT, .arg = flags, .value = directory };
    return (int)path_call(&m, path);
}

int nos_open(const char *p, unsigned f)
{
    return nos_openat(NOS_AT_FDCWD, p, f);
}

int nos_openat_mode(int directory, const char *path, unsigned flags, unsigned mode)
{
    if (flags & ~0xfffffu)
        return -NOS_EINVAL;
    struct nos_message m = { .op = NOS_VFS_OPENAT_MODE,
        .arg = (intptr_t)(uint32_t)(flags | ((mode & 07777u) << 20)), .value = directory };
    return (int)path_call(&m, path);
}

int nos_mkdirat(int directory, const char *path, unsigned mode)
{
    struct nos_message m = { .op = NOS_VFS_MKDIRAT, .arg = directory, .value = mode & 07777u };
    return (int)path_call(&m, path);
}

int nos_truncate(const char *path, int64_t length)
{
    if (length < 0)
        return -NOS_EINVAL;
    if ((uint64_t)length > INTPTR_MAX)
        return -NOS_EFBIG;
    struct nos_message m = { .op = NOS_VFS_TRUNCATE, .arg = (intptr_t)length };
    return (int)path_call(&m, path);
}

int nos_chmod_at(int directory, const char *path, unsigned mode)
{
    struct nos_message m = { .op = NOS_VFS_CHMOD, .arg = directory, .value = mode & 07777u };
    return (int)path_call(&m, path);
}

int nos_access_at(int directory, const char *path, unsigned mode, unsigned flags)
{
#ifdef NOS_LINUX_COMPAT
    if (mode & ~7u || flags & ~(NOS_AT_EACCESS | NOS_AT_SYMLINK_NOFOLLOW))
        return -NOS_EINVAL;
    struct nos_message m = { .op = NOS_VFS_ACCESS, .arg = directory,
        .value = mode | (flags << 8) };
    return (int)path_call(&m, path);
#else
    (void)directory;
    (void)path;
    (void)mode;
    (void)flags;
    return -NOS_ENOTSUP;
#endif
}

int nos_chown_at(int directory, const char *path, uint32_t uid, uint32_t gid, unsigned flags)
{
#ifdef NOS_LINUX_COMPAT
    if (flags & ~NOS_AT_SYMLINK_NOFOLLOW)
        return -NOS_EINVAL;
    PATH_SNAPSHOT(snapshot);
    int rc = path_snapshot_copy(&snapshot, path);
    if (rc)
        return rc;
    uint32_t ids[2] = {uid, gid};
    /* Ownership metadata occupies the inline payload. The path always uses
     * a token-bound input span, including short paths. */
    struct nos_message m = { .op = NOS_VFS_CHOWN, .arg = directory, .value = flags,
        .len = sizeof(ids), .bulk_in = (uintptr_t)snapshot.data, .bulk_in_len = snapshot.length };
    u_memcpy(m.data, ids, sizeof(ids));
    return (int)nos_rpc(&m);
#else
    (void)directory;
    (void)path;
    (void)uid;
    (void)gid;
    (void)flags;
    return -NOS_ENOTSUP;
#endif
}

/* Two-path namespace operations. */
static int path_pair(struct nos_message *m, const char *first, const char *second)
{
#ifdef NOS_LINUX_COMPAT
    PATH_SNAPSHOT(a);
    PATH_SNAPSHOT(b);
    int rc = path_snapshot_copy(&a, first);
    if (rc)
        return rc;
    rc = path_snapshot_copy(&b, second);
    if (rc)
        return rc;
    if (a.length + b.length <= NOS_MSG_DATA) {
        u_memcpy(m->data, a.data, a.length);
        u_memcpy(m->data + a.length, b.data, b.length);
        m->len = a.length + b.length;
        return (int)nos_rpc(m);
    }
    uint32_t lengths[2] = {(uint32_t)a.length, (uint32_t)b.length};
    u_memcpy(m->data, lengths, sizeof(lengths));
    m->len = sizeof(lengths);
    m->bulk_in = (uintptr_t)a.data;
    m->bulk_in_len = a.length;
    struct nos_callv_args args = { .request = (uintptr_t)m, .reply = (uintptr_t)m,
        .inputs = {{(uintptr_t)a.data, a.length}, {(uintptr_t)b.data, b.length}},
        .input_count = 2 };
    intptr_t result = nos_syscall(NOS_SYS_IPC_VECTOR, NOS_IPC_VECTOR_CALL, (uintptr_t)&args, 0);
    return (int)(result < 0 ? result : m->value);
#else
    char paths[2 * NOS_PATH_MAX];
    size_t first_size, second_size;
    int rc = copy_path(paths, first, &first_size);
    if (rc)
        return rc;
    rc = copy_path(paths + first_size, second, &second_size);
    if (rc)
        return rc;
    size_t total = first_size + second_size;
    if (total > NOS_MSG_DATA)
        return -NOS_ENAMETOOLONG;
    u_memcpy(m->data, paths, total);
    m->len = total;
    return (int)nos_rpc(m);
#endif
}

int nos_symlink_at(const char *target, int directory, const char *path)
{
    struct nos_message m = { .op = NOS_VFS_SYMLINK, .arg = directory };
    return path_pair(&m, target, path);
}

int nos_link_at(int old_directory, const char *old_path, int new_directory, const char *new_path)
{
    struct nos_message m = { .op = NOS_VFS_LINK, .arg = old_directory, .value = new_directory };
    return path_pair(&m, old_path, new_path);
}

int nos_unlinkat(int directory, const char *path, unsigned flags)
{
    struct nos_message m = { .op = NOS_VFS_UNLINKAT, .arg = directory, .value = flags };
    return (int)path_call(&m, path);
}

int nos_renameat(int old_directory, const char *old_path, int new_directory, const char *new_path)
{
    struct nos_message m = { .op = NOS_VFS_RENAMEAT, .arg = old_directory, .value = new_directory };
    return path_pair(&m, old_path, new_path);
}

int nos_mkdir(const char *p)
{
    return path_request(NOS_VFS_MKDIR, p, 0);
}

int nos_unlink(const char *p)
{
    return path_request(NOS_VFS_UNLINK, p, 0);
}

/* Path status, current directory, and link resolution. */
int nos_stat_at(int directory, const char *path, unsigned flags, struct nos_stat64 *st)
{
    int rc = nos_user_check(st, sizeof(*st), NOS_PROT_WRITE);
    if (rc)
        return rc;
    struct nos_message m = { .op = NOS_VFS_STATAT, .arg = directory, .value = flags };
    intptr_t result = path_call(&m, path);
    if (result)
        return (int)result;
    if (m.len != sizeof(*st))
        return -NOS_EIO;
    return nos_user_copy(st, m.data, sizeof(*st));
}

intptr_t nos_getcwd(char *buffer, size_t capacity)
{
    if (!capacity)
        return -NOS_ERANGE;
    size_t span = capacity < NOS_PATH_MAX ? capacity : NOS_PATH_MAX;
#ifdef NOS_LINUX_COMPAT
    PATH_SNAPSHOT(output);
    int rc = path_output_prepare(&output, span);
#else
    int rc = nos_user_check(buffer, span, NOS_PROT_WRITE);
#endif
    if (rc)
        return rc;
    struct nos_message m = { .op = NOS_VFS_GETCWD };
#ifdef NOS_LINUX_COMPAT
    m.bulk_out = (uintptr_t)output.data;
    m.bulk_out_len = span;
#endif
    intptr_t result = nos_rpc(&m);
    if (result < 0)
        return result;
#ifdef NOS_LINUX_COMPAT
    if (!result || (size_t)result > span || m.len)
        return -NOS_EIO;
    if (output.data[result - 1])
        return -NOS_EIO;
    /* Only the actual result must be writable, even when capacity extends
     * past a guard. The private IPC span satisfies whole-span preflight. */
    rc = nos_user_copy(buffer, output.data, (size_t)result);
    return rc ? rc : result;
#else
    if (!m.len || m.len > NOS_MSG_DATA || m.data[m.len - 1])
        return -NOS_EIO;
    if (m.len > capacity)
        return -NOS_ERANGE;
    rc = nos_user_copy(buffer, m.data, m.len);
    return rc ? rc : (intptr_t)m.len;
#endif
}

int nos_chdir(const char *path)
{
    return path_request(NOS_VFS_CHDIR, path, 0);
}

intptr_t nos_readlink_at(int directory, const char *path, char *buffer, size_t capacity)
{
    if (!capacity)
        return -NOS_EINVAL;
    size_t span = capacity < NOS_PATH_MAX ? capacity : NOS_PATH_MAX;
#ifdef NOS_LINUX_COMPAT
    PATH_SNAPSHOT(output);
    int rc = path_output_prepare(&output, span);
#else
    int rc = nos_user_check(buffer, span, NOS_PROT_WRITE);
#endif
    if (rc)
        return rc;
    struct nos_message m = { .op = NOS_VFS_READLINK, .arg = directory };
#ifdef NOS_LINUX_COMPAT
    m.bulk_out = (uintptr_t)output.data;
    m.bulk_out_len = span;
#endif
    intptr_t n = path_call(&m, path);
    if (n < 0)
        return n;
#ifdef NOS_LINUX_COMPAT
    if ((size_t)n > span || m.len)
        return -NOS_EIO;
    rc = nos_user_copy(buffer, output.data, (size_t)n);
    return rc ? rc : n;
#else
    if ((size_t)n > m.len || m.len > NOS_MSG_DATA)
        return -NOS_EIO;
    size_t copied = (size_t)n < capacity ? (size_t)n : capacity;
    rc = nos_user_copy(buffer, m.data, copied);
    return rc ? rc : (intptr_t)copied;
#endif
}

static intptr_t resolve_at(int directory, const char *path, char *buffer, size_t capacity, unsigned flags)
{
#ifdef NOS_LINUX_COMPAT
    if (!capacity)
        return -NOS_ERANGE;
    size_t span = capacity < NOS_PATH_MAX ? capacity : NOS_PATH_MAX;
    struct nos_message m = { .op = NOS_VFS_RESOLVE_PATH, .arg = directory, .value = flags,
        .bulk_out = (uintptr_t)buffer, .bulk_out_len = span };
    intptr_t count = path_call(&m, path);
    if (count < 0)
        return count;
    if (!count || (size_t)count > span || m.len)
        return -NOS_EIO;
    char terminator;
    int rc = nos_user_copy(&terminator, buffer + count - 1, 1);
    return rc ? rc : terminator ? -NOS_EIO : count;
#else
    (void)directory;
    (void)path;
    (void)buffer;
    (void)capacity;
    (void)flags;
    return -NOS_ENOTSUP;
#endif
}

intptr_t nos_resolve_at(int directory, const char *path, char *buffer, size_t capacity)
{
    return resolve_at(directory, path, buffer, capacity, 0);
}

intptr_t nos_resolve_exec_at(int directory, const char *path, char *buffer, size_t capacity)
{
    return resolve_at(directory, path, buffer, capacity, NOS_RESOLVE_EXEC);
}

int nos_rmdir(const char *path)
{
    return path_request(NOS_VFS_RMDIR, path, 0);
}

int nos_mount(const char *path, unsigned backend)
{
    return path_request(NOS_VFS_MOUNT, path, backend);
}

int nos_rename(const char *old_path, const char *new_path)
{
    struct nos_message m = { .op = NOS_VFS_RENAME };
    return path_pair(&m, old_path, new_path);
}
