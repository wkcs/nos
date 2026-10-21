/* Copyright (C) 2026 NOS contributors. Private bounded VFS pathname storage. */
#ifndef NOS_VFS_PATH_STORAGE_H
#define NOS_VFS_PATH_STORAGE_H
#ifdef NOS_LINUX_COMPAT
#define VFS_PATH_SCRATCH_BYTES (64u * 1024u)
#define VFS_NAME_BYTES (NODE_COUNT * NOS_PATH_MAX)
static char *path_storage, *path_scratch;
static size_t path_scratch_used, path_scratch_highwater;
struct path_scope { size_t mark; };
static int path_storage_initialize(void)
{
    if (path_storage) return 0;
    intptr_t mapped = nos_syscall(NOS_SYS_MMAP, 3, VFS_NAME_BYTES + VFS_PATH_SCRATCH_BYTES, 0);
    if (nos_pointer_is_error(mapped)) return (int)mapped;
    path_storage = (char *)(uintptr_t)mapped; path_scratch = path_storage + VFS_NAME_BYTES;
    return 0;
}
static void path_scope_end(struct path_scope *scope)
{ path_scratch_used = scope->mark; }
static char *path_scratch_take(size_t bytes)
{
    size_t aligned = (bytes + 15u) & ~(size_t)15u;
    if (!path_scratch || aligned < bytes || aligned > VFS_PATH_SCRATCH_BYTES - path_scratch_used) return NULL;
    char *result = path_scratch + path_scratch_used; path_scratch_used += aligned;
    if (path_scratch_used > path_scratch_highwater) path_scratch_highwater = path_scratch_used;
    return result;
}
#define PATH_SCOPE struct path_scope path_scope __attribute__((cleanup(path_scope_end))) = { path_scratch_used }
#define PATH_BUFFER(name,bytes) char *name = path_scratch_take(bytes)
#define PATH_MISSING(name) (!(name))
#else
#define PATH_SCOPE
#define PATH_BUFFER(name,bytes) char name[bytes]
#define PATH_MISSING(name) 0
#endif
#define VFS_PERSIST_PATH_MAX 64u
#define VFS_ARCHIVE_PATH_MAX NOS_BOOTFILE_PATH_MAX
#ifdef NOS_LINUX_COMPAT
#define VFS_DIRENT_NAME_BYTES (NOS_NAME_MAX + 1u)
#else
#define VFS_DIRENT_NAME_BYTES NOS_PATH_MAX
#endif
#ifndef NOS_NAME_MAX
#define NOS_NAME_MAX 255u
#endif
#endif
