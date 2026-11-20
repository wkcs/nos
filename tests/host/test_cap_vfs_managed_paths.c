/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Author: Nick Hu
 *
 * Invoke the production capability RPC dispatcher. Mock only its OS boundary so
 * denied requests must fail before any filesystem mutation can be attempted.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <nos/nlk_caps.h>
#include <uapi/nos/errno.h>

static struct nlk_node node;
static struct nlk_vfs_ctx context;
static unsigned int opens, mutations;

static bool descriptor_open;
static int descriptor_flags;
static struct inode inode = {.i_mode = S_IFREG | 0644, .i_size = 3};
static struct dentry entry = {.d_inode = &inode};

int nlk_node_register_cap(struct nlk_node *target, struct nlk_capability *cap)
{
    assert(target == &node && cap == &context.cap);
    return 0;
}

int nos_open(const char *path, int flags, ...)
{
    assert(path != NULL && path[0] == '/' && !descriptor_open);
    opens++;
    if ((flags & (O_ACCMODE | O_CREAT | O_TRUNC | O_APPEND)) != O_RDONLY) mutations++;
    descriptor_open = true;
    descriptor_flags = flags;
    return 7;
}

int nos_close(int fd)
{
    assert(fd == 7 && descriptor_open);
    descriptor_open = false;
    return 0;
}

loff_t nos_lseek(int fd, loff_t offset, int whence)
{
    assert(fd == 7 && descriptor_open && whence == SEEK_SET);
    return offset;
}

ssize_t nos_read(int fd, void *buffer, size_t length)
{
    assert(fd == 7 && descriptor_open && length == 3U);
    memcpy(buffer, "NKC", length);
    return (ssize_t)length;
}

ssize_t nos_write(int fd, const void *buffer, size_t length)
{
    assert(fd == 7 && descriptor_open && buffer != NULL);
    if ((descriptor_flags & O_ACCMODE) == O_RDONLY) return -EBADF;
    mutations++;
    return (ssize_t)length;
}

int nos_mkdir(const char *path, mode_t mode)
{
    assert(path != NULL && mode != 0U);
    mutations++;
    return 0;
}

int nos_unlink(const char *path)
{
    assert(path != NULL);
    mutations++;
    return 0;
}

int nos_rmdir(const char *path)
{
    assert(path != NULL);
    mutations++;
    return 0;
}

int nos_rename(const char *old_path, const char *new_path)
{
    assert(old_path != NULL && new_path != NULL);
    mutations++;
    return 0;
}

struct dentry *vfs_get_root(void) { return &entry; }

struct dentry *vfs_lookup_path(struct dentry *root, const char *path)
{
    assert(root == &entry && path != NULL);
    inode.i_mode = strstr(path, "/directory") != NULL ? S_IFDIR | 0755 : S_IFREG | 0644;
    return &entry;
}

struct dentry *vfs_find_cached_child(struct dentry *parent, const char *name, unsigned int length)
{
    (void)parent;
    (void)name;
    (void)length;
    return NULL;
}

int vfs_get_mount_info(int index, struct vfs_mount_info *info)
{
    (void)index;
    (void)info;
    return -ENOENT;
}

static int rpc(uint8_t operation, const void *request, uint16_t size, void *response,
                uint16_t *response_size)
{
    struct nlk_pkt_hdr header = {.cmd_op = operation};
    int rc = context.cap.ops->on_rpc(&node, &context.cap, &header,
                                    request, size, response, response_size);
    return rc;
}

static int open_path(const char *path, uint16_t flags, int32_t *handle)
{
    struct nlk_vfs_open_req request = {.flags = flags, .mode = 0644};
    struct nlk_vfs_open_resp response;
    uint16_t size;
    int ret;
    assert(strlen(path) < sizeof(request.path));
    strcpy(request.path, path);
    ret = rpc(NLK_VFS_OP_OPEN, &request, sizeof(request), &response, &size);
    if (ret == 0) {
        assert(size == sizeof(response));
        *handle = response.handle;
    } else {
        assert(size == 0U);
    }
    return ret;
}

static void close_handle(int32_t handle)
{
    struct nlk_vfs_close_req request = {.handle = handle};
    uint16_t size;
    assert(rpc(NLK_VFS_OP_CLOSE, &request, sizeof(request), NULL, &size) == 0);
}

static int path_rpc(uint8_t operation, const char *path)
{
    struct nlk_vfs_path_req request = {.mode = 0755};
    uint8_t response[CONFIG_NLK_PACKET_MTU];
    uint16_t size;
    assert(strlen(path) < sizeof(request.path));
    strcpy(request.path, path);
    return rpc(operation, &request, sizeof(request), response, &size);
}

static int rename_path(const char *old_path, const char *new_path)
{
    struct nlk_vfs_rename_req request = {0};
    uint16_t size;
    assert(strlen(old_path) < sizeof(request.old_path));
    assert(strlen(new_path) < sizeof(request.new_path));
    strcpy(request.old_path, old_path);
    strcpy(request.new_path, new_path);
    return rpc(NLK_VFS_OP_RENAME, &request, sizeof(request), NULL, &size);
}

static void read_handle(int32_t handle)
{
    struct nlk_vfs_read_req request = {.handle = handle, .count = 3};
    uint8_t response[CONFIG_NLK_PACKET_MTU];
    struct nlk_vfs_read_resp_hdr result;
    uint16_t size;
    assert(rpc(NLK_VFS_OP_READ, &request, sizeof(request), response, &size) == 0);
    memcpy(&result, response, sizeof(result));
    assert(result.bytes_read == 3U && size == sizeof(result) + 3U);
    assert(memcmp(response + sizeof(result), "NKC", 3) == 0);
}

static void write_handle(int32_t handle, int expected)
{
    uint8_t request[sizeof(struct nlk_vfs_write_req_hdr) + 1U] = {0};
    struct nlk_vfs_write_req_hdr header = {.handle = handle, .count = 1};
    struct nlk_vfs_write_resp response;
    uint16_t size;
    memcpy(request, &header, sizeof(header));
    request[sizeof(header)] = 'x';
    assert(rpc(NLK_VFS_OP_WRITE, request, sizeof(request), &response, &size) == expected);
    if (expected == 0) assert(response.bytes_written == 1U);
}

int main(void)
{
    static const char *const managed[] = {
        "/data/service", "/data/service/config.nkc", "/data/service/config.tmp",
        "/data/service/object.tmp",
        "/data/service/base", "/data/service/light/profile.nkc",
        "/data/service/macro/profile.nkc",
        "//data///service//base///default.nkc",
        "/data/./service/config.nkc", "/data/tmp/../service/config.nkc",
        "/x/../data/service/config.nkc", "/data/service/./nested/../config.nkc",
        "data/service/config.nkc", "/../../data/service/config.nkc"
    };
    static const char *const ancestors[] = {
        "/", "///", "/.", "/data", "/data/", "/data/.",
        "/data/service/..", "/data/service/../..",
        "/unrelated/../data", "/data/../", "/data/../../"
    };
    static const char *const unrelated[] = {
        "/data/service/active.nkc", "/data/service/unused.txt",
        "/data/service/base-backup/file", "/data/service/config.nkc.bak",
        "/data/service/./nested/../active.nkc",
        "/backup/file", "backup/relative", "/data/service-backup/file", "/data/services/file",
        "/database/keyboard/file", "/data/service/../notes", "/data/directory"
    };
    static const uint16_t flags[] = {
        NLK_VFS_O_WRONLY, NLK_VFS_O_RDWR,
        NLK_VFS_O_RDONLY | NLK_VFS_O_CREAT, NLK_VFS_O_RDONLY | NLK_VFS_O_TRUNC,
        NLK_VFS_O_RDONLY | NLK_VFS_O_APPEND,
        NLK_VFS_O_RDWR | NLK_VFS_O_CREAT | NLK_VFS_O_TRUNC
    };
    int32_t handle = -1;

    static const struct nlk_vfs_protected_path rules[] = {
        {"/data/service/config.nkc", false},
        {"/data/service/config.tmp", false},
        {"/data/service/object.tmp", false},
        {"/data/service/base", true},
        {"/data/service/light", true},
        {"/data/service/macro", true},
    };
    static const struct nlk_vfs_protected_path second_rules[] = {
        {"/other/owned", true},
    };
    static struct nlk_vfs_write_policy policy = {
        .paths = rules,
        .count = sizeof(rules) / sizeof(rules[0]),
    };
    static struct nlk_vfs_write_policy second_policy = {
        .paths = second_rules,
        .count = sizeof(second_rules) / sizeof(second_rules[0]),
    };

    assert(nlk_cap_vfs_init(&node, &context) == 0);
    assert(path_rpc(NLK_VFS_OP_REMOVE, "/data/service/config.nkc") == 0);
    nlk_cap_vfs_add_write_policy(&policy);
    nlk_cap_vfs_add_write_policy(&second_policy);
    nlk_cap_vfs_add_write_policy(&policy);
    assert(path_rpc(NLK_VFS_OP_REMOVE, "/OTHER/OWNED/file") == -EPERM);
    assert(path_rpc(NLK_VFS_OP_REMOVE, "/DATA/Service/CONFIG.NKC") == -EPERM);
    for (size_t i = 0; i < sizeof(managed) / sizeof(managed[0]); i++) {
        unsigned int before_mutations = mutations, before_opens = opens;
        for (size_t j = 0; j < sizeof(flags) / sizeof(flags[0]); j++) {
            assert(open_path(managed[i], flags[j], &handle) == -EPERM);
        }
        assert(path_rpc(NLK_VFS_OP_MKDIR, managed[i]) == -EPERM);
        assert(path_rpc(NLK_VFS_OP_REMOVE, managed[i]) == -EPERM);
        assert(rename_path(managed[i], "/backup/file") == -EPERM);
        assert(rename_path("/backup/file", managed[i]) == -EPERM);
        assert(mutations == before_mutations && opens == before_opens);

        assert(open_path(managed[i], NLK_VFS_O_RDONLY, &handle) == 0);
        read_handle(handle);
        write_handle(handle, -EBADF);
        close_handle(handle);
        assert(path_rpc(NLK_VFS_OP_STAT, managed[i]) == 0);
        assert(mutations == before_mutations);
    }
    for (size_t i = 0; i < sizeof(ancestors) / sizeof(ancestors[0]); i++) {
        unsigned int before_mutations = mutations;
        assert(open_path(ancestors[i], NLK_VFS_O_WRONLY | NLK_VFS_O_TRUNC,
                         &handle) == -EPERM);
        assert(path_rpc(NLK_VFS_OP_MKDIR, ancestors[i]) == -EPERM);
        assert(path_rpc(NLK_VFS_OP_REMOVE, ancestors[i]) == -EPERM);
        assert(rename_path(ancestors[i], "/backup/file") == -EPERM);
        assert(rename_path("/backup/file", ancestors[i]) == -EPERM);
        assert(mutations == before_mutations);
    }
    for (size_t i = 0; i < sizeof(unrelated) / sizeof(unrelated[0]); i++) {
        unsigned int before_mutations = mutations;
        assert(open_path(unrelated[i], NLK_VFS_O_RDWR | NLK_VFS_O_CREAT, &handle) == 0);
        write_handle(handle, 0);
        close_handle(handle);
        assert(path_rpc(NLK_VFS_OP_MKDIR, unrelated[i]) == 0);
        assert(path_rpc(NLK_VFS_OP_REMOVE, unrelated[i]) == 0);
        assert(rename_path(unrelated[i], "/backup/new") == 0);
        assert(rename_path("/backup/old", unrelated[i]) == 0);
        assert(mutations == before_mutations + 6U);
    }
    assert(!descriptor_open);
    puts("cap_vfs managed path guard tests passed");
    return 0;
}
