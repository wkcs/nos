/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <string.h>
#include "glob.h"
#include "glob_vfs.h"

#define GLOB_PATH_MAX 256

void shell_glob_free(struct shell_glob_result *result)
{
    while (result != NULL) {
        struct shell_glob_result *next = result->next;
        kfree(result);
        result = next;
    }
}

static int glob_add(struct shell_glob_result **list, const char *prefix,
                    const char *name, size_t length, bool slash)
{
    size_t prefix_len = strlen(prefix);
    size_t separator = prefix_len != 0 && prefix[prefix_len - 1] != '/';
    struct shell_glob_result *node, **slot;
    if (prefix_len + separator + length + slash > GLOB_PATH_MAX) return -ENAMETOOLONG;
    node = kmalloc(sizeof(*node) + prefix_len + separator + length + slash + 1, GFP_KERNEL);
    if (node == NULL) return -ENOMEM;
    memcpy(node->path, prefix, prefix_len);
    if (separator) node->path[prefix_len++] = '/';
    memcpy(node->path + prefix_len, name, length);
    prefix_len += length;
    if (slash) node->path[prefix_len++] = '/';
    node->path[prefix_len] = '\0';
    slot = list;
    while (*slot != NULL && strcmp((*slot)->path, node->path) <= 0) slot = &(*slot)->next;
    node->next = *slot;
    *slot = node;
    return 0;
}

struct glob_context {
    const char *prefix;
    const char *component;
    const unsigned char *quoted;
    struct shell_glob_result **next;
    char *entry;
    int error;
};

static int glob_filldir(void *data, const char *name, int length, loff_t offset,
                        u64 ino, unsigned type)
{
    struct glob_context *ctx = data;
    (void)offset;
    (void)ino;
    (void)type;
    if (ctx->error != 0 || length < 0) return 1;
    if (length >= FS_NAME_LEN) return 0;
    memcpy(ctx->entry, name, (size_t)length);
    ctx->entry[length] = '\0';
    if (shell_glob_match(ctx->component, ctx->quoted, ctx->entry)) {
        ctx->error = glob_add(ctx->next, ctx->prefix, ctx->entry, (size_t)length, false);
    }
    return ctx->error != 0;
}

static int glob_directory(struct dentry *dir, struct glob_context *ctx)
{
    struct file *file;
    int rc;
    if (dir == NULL || dir->d_inode == NULL || !S_ISDIR(dir->d_inode->i_mode) ||
        dir->d_inode->i_fop == NULL || dir->d_inode->i_fop->readdir == NULL) return 0;
    file = kzalloc(sizeof(*file), GFP_KERNEL);
    if (file == NULL) return -ENOMEM;
    file->f_inode = dir->d_inode;
    file->f_op = dir->d_inode->i_fop;
    file->f_path.dentry = dir;
    rc = file->f_op->open != NULL ? file->f_op->open(file->f_inode, file) : 0;
    if (rc == 0) {
        rc = file->f_op->readdir(file, ctx, glob_filldir);
        if (file->f_op->release != NULL) {
            int release_rc = file->f_op->release(file->f_inode, file);
            if (rc == 0) rc = release_rc;
        }
    }
    kfree(file);
    return ctx->error != 0 ? ctx->error : rc;
}

int shell_glob_expand(struct dentry *cwd, const char *pattern, const unsigned char *quoted,
                      struct shell_glob_result **result)
{
    struct shell_glob_result *candidates = NULL, *next = NULL, *item;
    struct glob_workspace {
        char entry[FS_NAME_LEN];
        char data[];
    } *workspace;
    char *component;
    unsigned char *mask;
    size_t pos = 0;
    int rc;
    bool trailing;
    *result = NULL;
    workspace = kmalloc(sizeof(*workspace) + 2 * (strlen(pattern) + 1), GFP_KERNEL);
    if (workspace == NULL) return -ENOMEM;
    component = workspace->data;
    mask = (unsigned char *)(workspace->data + strlen(pattern) + 1);
    rc = glob_add(&candidates, "", pattern[0] == '/' ? "/" : "", pattern[0] == '/' ? 1 : 0, false);
    if (rc != 0) goto out;
    while (pattern[pos] == '/') pos++;
    while (pattern[pos] != '\0' && candidates != NULL) {
        size_t start = pos, length, i;
        bool magic = false;
        while (pattern[pos] != '\0' && pattern[pos] != '/') pos++;
        length = pos - start;
        if (length > GLOB_PATH_MAX) { rc = -ENAMETOOLONG; goto out; }
        memcpy(component, pattern + start, length);
        component[length] = '\0';
        for (i = 0; i < length; i++) {
            mask[i] = quoted != NULL ? quoted[start + i] : 0;
            if (!mask[i] && (component[i] == '*' || component[i] == '?' || component[i] == '[')) magic = true;
        }
        mask[length] = 0;
        trailing = pattern[pos] == '/';
        while (pattern[pos] == '/') pos++;
        for (item = candidates; item != NULL; item = item->next) {
            if (magic) {
                struct dentry *dir = item->path[0] == '\0' ? cwd : vfs_lookup_path(cwd, item->path);
                struct glob_context ctx = { item->path, component, mask, &next, workspace->entry, 0 };
                rc = glob_directory(dir, &ctx);
                if (rc != 0) goto out;
            } else {
                struct shell_glob_result *single = NULL;
                struct dentry *found;
                rc = glob_add(&single, item->path, component, length, false);
                if (rc != 0) goto out;
                found = vfs_lookup_path(cwd, single->path);
                if (found != NULL && found->d_inode != NULL) {
                    rc = glob_add(&next, item->path, component, length, false);
                }
                shell_glob_free(single);
                if (rc != 0) goto out;
            }
        }
        shell_glob_free(candidates);
        candidates = next;
        next = NULL;
        if (trailing && pattern[pos] == '\0') {
            for (item = candidates; item != NULL; item = item->next) {
                struct dentry *dir = vfs_lookup_path(cwd, item->path);
                if (dir != NULL && dir->d_inode != NULL && S_ISDIR(dir->d_inode->i_mode)) {
                    rc = glob_add(&next, item->path, "", 0, false);
                    if (rc != 0) goto out;
                }
            }
            shell_glob_free(candidates);
            candidates = next;
            next = NULL;
        }
    }
    *result = candidates;
    kfree(workspace);
    return 0;
out:
    shell_glob_free(candidates);
    shell_glob_free(next);
    kfree(workspace);
    return rc;
}
