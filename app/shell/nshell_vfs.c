/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * VFS path normalization, glob matching, and file/directory operations for nShell.
 */

#include "nshell_internal.h"
#include <kernel/errno.h>
#include <kernel/mm.h>
#include <lib/vsprintf.h>
#include <string.h>

void nshell_init_cwd(struct nshell *shell)
{
    if (shell == NULL) {
        return;
    }
    if (shell->session.cwd == NULL) {
        shell->session.cwd = vfs_get_root();
    }
    if (shell->cwd_path[0] == '\0') {
        strcpy(shell->cwd_path, "/");
    }
    if (shell->oldpwd_path[0] == '\0') {
        strcpy(shell->oldpwd_path, "/");
    }
}

const char *nshell_basename(const char *path)
{
    const char *last;

    if (path == NULL || path[0] == '\0') {
        return ".";
    }
    last = path + strlen(path);
    while (last > path && *(last - 1) == '/') {
        last--;
    }
    if (last == path) {
        return "/";
    }
    while (last > path && *(last - 1) != '/') {
        last--;
    }
    return last;
}

int nshell_normalize_path(const char *cwd, const char *input,
                          char *out, size_t out_size)
{
    const char *p;
    size_t out_len = 0U;

    if (out == NULL || out_size < 2U) {
        return -EINVAL;
    }
    if (input == NULL || input[0] == '\0') {
        input = ".";
    }
    if (input[0] == '/') {
        out[0] = '/';
        out[1] = '\0';
        out_len = 1U;
        p = input + 1;
    } else {
        const char *base = (cwd != NULL && cwd[0] == '/') ? cwd : "/";
        size_t base_len = strlen(base);
        if (base_len >= out_size) {
            return -ENAMETOOLONG;
        }
        memcpy(out, base, base_len + 1U);
        out_len = base_len;
        while (out_len > 1U && out[out_len - 1U] == '/') {
            out[--out_len] = '\0';
        }
        p = input;
    }

    while (*p != '\0') {
        const char *start;
        size_t comp_len = 0U;

        while (*p == '/') {
            p++;
        }
        if (*p == '\0') {
            break;
        }
        start = p;
        while (*p != '\0' && *p != '/') {
            p++;
            comp_len++;
        }
        if (comp_len == 1U && start[0] == '.') {
            continue;
        }
        if (comp_len == 2U && start[0] == '.' && start[1] == '.') {
            if (out_len > 1U) {
                while (out_len > 1U && out[out_len - 1U] != '/') {
                    out_len--;
                }
                if (out_len > 1U && out[out_len - 1U] == '/') {
                    out_len--;
                }
                out[out_len] = '\0';
            }
            continue;
        }
        if (comp_len >= FS_NAME_LEN) {
            return -ENAMETOOLONG;
        }
        if (out_len > 1U) {
            if (out_len + 1U + comp_len >= out_size) {
                return -ENAMETOOLONG;
            }
            out[out_len++] = '/';
        } else if (out_len == 1U && out[0] == '/') {
            if (1U + comp_len >= out_size) {
                return -ENAMETOOLONG;
            }
        }
        memcpy(out + out_len, start, comp_len);
        out_len += comp_len;
        out[out_len] = '\0';
    }
    if (out_len == 0U) {
        out[0] = '/';
        out[1] = '\0';
    }
    return 0;
}

struct dentry *nshell_resolve_path(struct nshell *shell, const char *path,
                                   char *norm_out, size_t norm_size)
{
    char local_norm[SHELL_PATH_MAX];
    char *buf = norm_out != NULL ? norm_out : local_norm;
    size_t cap = norm_out != NULL ? norm_size : sizeof(local_norm);
    const char *cwd = "/";

    if (shell != NULL) {
        nshell_init_cwd(shell);
        cwd = shell->cwd_path;
    }
    if (nshell_normalize_path(cwd, path, buf, cap) != 0) {
        return NULL;
    }
    return vfs_lookup_path(vfs_get_root(), buf);
}

int nshell_resolve_parent(struct nshell *shell, const char *path,
                          struct dentry **parent_out, char *name_out,
                          size_t name_size, char *norm_out, size_t norm_size)
{
    char local_norm[SHELL_PATH_MAX];
    char *norm = norm_out != NULL ? norm_out : local_norm;
    size_t cap = norm_out != NULL ? norm_size : sizeof(local_norm);
    const char *cwd = "/";
    const char *slash;
    size_t name_len;
    int rc;

    if (parent_out == NULL || name_out == NULL || name_size == 0U) {
        return -EINVAL;
    }
    if (shell != NULL) {
        nshell_init_cwd(shell);
        cwd = shell->cwd_path;
    }
    rc = nshell_normalize_path(cwd, path, norm, cap);
    if (rc != 0) {
        return rc;
    }
    if (strcmp(norm, "/") == 0) {
        return -EINVAL;
    }
    slash = norm + strlen(norm);
    while (slash > norm && *(slash - 1) != '/') {
        slash--;
    }
    name_len = strlen(slash);
    if (name_len == 0U || name_len >= name_size || name_len >= FS_NAME_LEN) {
        return -ENAMETOOLONG;
    }
    memcpy(name_out, slash, name_len + 1U);

    if (slash == norm + 1) {
        *parent_out = vfs_get_root();
    } else {
        size_t dir_len = (size_t)(slash - norm - 1);
        char *dir_path = kmalloc((u32)(dir_len + 1U), GFP_KERNEL);
        if (dir_path == NULL) {
            return -ENOMEM;
        }
        memcpy(dir_path, norm, dir_len);
        dir_path[dir_len] = '\0';
        *parent_out = vfs_lookup_path(vfs_get_root(), dir_path);
        kfree(dir_path);
    }
    if (*parent_out == NULL || (*parent_out)->d_inode == NULL) {
        return -ENOENT;
    }
    if (!S_ISDIR((*parent_out)->d_inode->i_mode)) {
        return -ENOTDIR;
    }
    return 0;
}

int nshell_read_file_dentry(struct dentry *dentry, char **buf_out, size_t *len_out)
{
    struct file *f;
    char *buf = NULL;
    size_t cap = 256U;
    size_t total = 0U;
    int rc = 0;

    if (buf_out == NULL || len_out == NULL) {
        return -EINVAL;
    }
    *buf_out = NULL;
    *len_out = 0U;

    if (dentry == NULL || dentry->d_inode == NULL) {
        return -ENOENT;
    }
    if (S_ISDIR(dentry->d_inode->i_mode)) {
        return -EISDIR;
    }
    if (dentry->d_inode->i_fop == NULL || dentry->d_inode->i_fop->read == NULL) {
        return -EACCES;
    }
    if (dentry->d_inode->i_size > 0 && dentry->d_inode->i_size < 65536) {
        cap = (size_t)dentry->d_inode->i_size + 16U;
    }
    buf = kmalloc((u32)(cap + 1U), GFP_KERNEL);
    if (buf == NULL) {
        return -ENOMEM;
    }
    f = kzalloc(sizeof(*f), GFP_KERNEL);
    if (f == NULL) {
        kfree(buf);
        return -ENOMEM;
    }
    f->f_inode = dentry->d_inode;
    f->f_op = dentry->d_inode->i_fop;
    f->f_path.dentry = dentry;
    f->f_flags = O_RDONLY;
    f->f_pos = 0;

    if (f->f_op->open != NULL) {
        rc = f->f_op->open(dentry->d_inode, f);
        if (rc != 0) {
            kfree(f);
            kfree(buf);
            return rc;
        }
    }

    for (;;) {
        ssize_t n;
        if (total + 64U > cap) {
            size_t new_cap = cap * 2U;
            char *new_buf;
            if (new_cap > 32768U) {
                rc = -EFBIG;
                break;
            }
            new_buf = krealloc(buf, (u32)(new_cap + 1U), GFP_KERNEL);
            if (new_buf == NULL) {
                rc = -ENOMEM;
                break;
            }
            buf = new_buf;
            cap = new_cap;
        }
        n = f->f_op->read(f, buf + total, cap - total, &f->f_pos);
        if (n < 0) {
            rc = (int)n;
            break;
        }
        if (n == 0) {
            break;
        }
        total += (size_t)n;
    }

    if (f->f_op->release != NULL) {
        (void)f->f_op->release(dentry->d_inode, f);
    }
    kfree(f);

    if (rc != 0) {
        kfree(buf);
        return rc;
    }
    buf[total] = '\0';
    *buf_out = buf;
    *len_out = total;
    return 0;
}

int nshell_read_file(struct nshell *shell, const char *path,
                     char **buf_out, size_t *len_out)
{
    struct dentry *dentry = nshell_resolve_path(shell, path, NULL, 0U);
    if (dentry == NULL) {
        return -ENOENT;
    }
    return nshell_read_file_dentry(dentry, buf_out, len_out);
}

int nshell_write_file(struct nshell *shell, const char *path,
                      const char *data, size_t len, bool append)
{
    struct dentry *parent = NULL;
    struct dentry *dentry = NULL;
    struct file *f;
    char name[FS_NAME_LEN];
    int rc;
    size_t written = 0U;

    rc = nshell_resolve_parent(shell, path, &parent, name, sizeof(name), NULL, 0U);
    if (rc != 0) {
        return rc;
    }
    rc = vfs_create_file(parent, name, 0644, &dentry);
    if (rc != 0 || dentry == NULL || dentry->d_inode == NULL) {
        return rc != 0 ? rc : -EIO;
    }
    if (S_ISDIR(dentry->d_inode->i_mode)) {
        return -EISDIR;
    }
    if (dentry->d_inode->i_fop == NULL || dentry->d_inode->i_fop->write == NULL) {
        return -EROFS;
    }
    if (!append) {
        (void)vfs_truncate_dentry(dentry, 0);
    }

    f = kzalloc(sizeof(*f), GFP_KERNEL);
    if (f == NULL) {
        return -ENOMEM;
    }
    f->f_inode = dentry->d_inode;
    f->f_op = dentry->d_inode->i_fop;
    f->f_path.dentry = dentry;
    f->f_flags = O_WRONLY | O_CREAT | (append ? O_APPEND : O_TRUNC);
    f->f_pos = append ? dentry->d_inode->i_size : 0;

    if (f->f_op->open != NULL) {
        rc = f->f_op->open(dentry->d_inode, f);
        if (rc != 0) {
            kfree(f);
            return rc;
        }
    }
    if (append) {
        f->f_pos = dentry->d_inode->i_size;
    }

    while (written < len) {
        ssize_t n = f->f_op->write(f, data + written, len - written, &f->f_pos);
        if (n <= 0) {
            rc = n < 0 ? (int)n : -EIO;
            break;
        }
        written += (size_t)n;
    }

    if (f->f_op->release != NULL) {
        (void)f->f_op->release(dentry->d_inode, f);
    }
    kfree(f);
    return rc;
}

int nshell_copy_file(struct nshell *shell, const char *src_path,
                     const char *dst_path, bool force)
{
    struct dentry *dst_existing;
    char *buf = NULL;
    size_t len = 0U;
    int rc;

    dst_existing = nshell_resolve_path(shell, dst_path, NULL, 0U);
    if (dst_existing != NULL) {
        if (dst_existing->d_inode != NULL && S_ISDIR(dst_existing->d_inode->i_mode)) {
            return -EISDIR;
        }
        if (!force) {
            return -EEXIST;
        }
    }
    rc = nshell_read_file(shell, src_path, &buf, &len);
    if (rc != 0) {
        return rc;
    }
    rc = nshell_write_file(shell, dst_path, buf, len, false);
    kfree(buf);
    return rc;
}

struct dir_item {
    char name[FS_NAME_LEN];
    unsigned int dtype;
};

struct dir_list_ctx {
    struct dir_item *items;
    size_t count;
    size_t cap;
};

static int collect_dir_cb(void *dirent, const char *name, int namelen,
                          loff_t offset, u64 ino, unsigned d_type)
{
    struct dir_list_ctx *ctx = dirent;
    size_t len;

    (void)offset;
    (void)ino;
    if (ctx == NULL || name == NULL) {
        return 0;
    }
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
        return 0;
    }
    if (ctx->count >= ctx->cap) {
        size_t new_cap = ctx->cap == 0U ? 8U : ctx->cap * 2U;
        struct dir_item *new_items;
        if (new_cap > 128U) {
            return -ENOMEM;
        }
        new_items = krealloc(ctx->items, (u32)(new_cap * sizeof(*new_items)), GFP_KERNEL);
        if (new_items == NULL) {
            return -ENOMEM;
        }
        ctx->items = new_items;
        ctx->cap = new_cap;
    }
    len = namelen > 0 ? (size_t)namelen : strlen(name);
    if (len >= FS_NAME_LEN) {
        len = FS_NAME_LEN - 1U;
    }
    memcpy(ctx->items[ctx->count].name, name, len);
    ctx->items[ctx->count].name[len] = '\0';
    ctx->items[ctx->count].dtype = d_type;
    ctx->count++;
    return 0;
}

static int read_dir_entries(struct dentry *dir, struct dir_list_ctx *ctx)
{
    struct file *f;
    int rc = 0;

    memset(ctx, 0, sizeof(*ctx));
    if (dir == NULL || dir->d_inode == NULL || !S_ISDIR(dir->d_inode->i_mode)) {
        return -ENOTDIR;
    }
    if (dir->d_inode->i_fop == NULL || dir->d_inode->i_fop->readdir == NULL) {
        return 0;
    }
    f = kzalloc(sizeof(*f), GFP_KERNEL);
    if (f == NULL) {
        return -ENOMEM;
    }
    f->f_inode = dir->d_inode;
    f->f_op = dir->d_inode->i_fop;
    f->f_path.dentry = dir;
    f->f_pos = 0;
    if (f->f_op->open != NULL) {
        rc = f->f_op->open(dir->d_inode, f);
    }
    if (rc == 0) {
        rc = f->f_op->readdir(f, ctx, collect_dir_cb);
    }
    if (f->f_op->release != NULL) {
        (void)f->f_op->release(dir->d_inode, f);
    }
    kfree(f);
    return rc;
}

static int copy_recursive_depth(struct nshell *shell, const char *src_path,
                                const char *dst_path, bool force, bool verbose,
                                int depth)
{
    struct dentry *src_dentry;
    struct dentry *dst_parent = NULL;
    struct dentry *dst_dir = NULL;
    char dst_name[FS_NAME_LEN];
    struct dir_list_ctx list = {0};
    size_t i;
    int rc;

    if (depth > 8) {
        return -ELOOP;
    }
    src_dentry = nshell_resolve_path(shell, src_path, NULL, 0U);
    if (src_dentry == NULL || src_dentry->d_inode == NULL) {
        return -ENOENT;
    }
    if (!S_ISDIR(src_dentry->d_inode->i_mode)) {
        rc = nshell_copy_file(shell, src_path, dst_path, force);
        if (rc == 0 && verbose) {
            shell_printf("'%s' -> '%s'\n", src_path, dst_path);
        }
        return rc;
    }

    dst_dir = nshell_resolve_path(shell, dst_path, NULL, 0U);
    if (dst_dir == NULL) {
        rc = nshell_resolve_parent(shell, dst_path, &dst_parent, dst_name,
                                   sizeof(dst_name), NULL, 0U);
        if (rc != 0) {
            return rc;
        }
        rc = vfs_make_dir(dst_parent, dst_name, 0755, &dst_dir);
        if (rc != 0 && rc != -EEXIST) {
            return rc;
        }
        if (verbose) {
            shell_printf("'%s' -> '%s'\n", src_path, dst_path);
        }
    } else if (!S_ISDIR(dst_dir->d_inode->i_mode)) {
        return -ENOTDIR;
    }

    rc = read_dir_entries(src_dentry, &list);
    if (rc != 0) {
        if (list.items) kfree(list.items);
        return rc;
    }
    for (i = 0U; i < list.count; i++) {
        char *child_src = kmalloc(SHELL_PATH_MAX, GFP_KERNEL);
        char *child_dst = kmalloc(SHELL_PATH_MAX, GFP_KERNEL);
        if (child_src == NULL || child_dst == NULL) {
            if (child_src) kfree(child_src);
            if (child_dst) kfree(child_dst);
            rc = -ENOMEM;
            break;
        }
        snprintf(child_src, SHELL_PATH_MAX, "%s/%s",
                 strcmp(src_path, "/") == 0 ? "" : src_path, list.items[i].name);
        snprintf(child_dst, SHELL_PATH_MAX, "%s/%s",
                 strcmp(dst_path, "/") == 0 ? "" : dst_path, list.items[i].name);
        rc = copy_recursive_depth(shell, child_src, child_dst, force, verbose, depth + 1);
        kfree(child_src);
        kfree(child_dst);
        if (rc != 0) {
            break;
        }
    }
    if (list.items) {
        kfree(list.items);
    }
    return rc;
}

int nshell_copy_recursive(struct nshell *shell, const char *src_path,
                          const char *dst_path, bool force, bool verbose)
{
    return copy_recursive_depth(shell, src_path, dst_path, force, verbose, 0);
}

static int remove_recursive_depth(struct nshell *shell, const char *path,
                                  bool force, bool verbose, int depth)
{
    struct dentry *target;
    struct dentry *parent = NULL;
    char name[FS_NAME_LEN];
    char norm[SHELL_PATH_MAX];
    int rc;

    if (depth > 8) {
        return -ELOOP;
    }
    target = nshell_resolve_path(shell, path, norm, sizeof(norm));
    if (target == NULL || target->d_inode == NULL) {
        return force ? 0 : -ENOENT;
    }
    if (strcmp(norm, "/") == 0) {
        return -EBUSY;
    }
    if (S_ISDIR(target->d_inode->i_mode)) {
        struct dir_list_ctx list = {0};
        size_t i;
        rc = read_dir_entries(target, &list);
        if (rc != 0) {
            if (list.items) kfree(list.items);
            return rc;
        }
        for (i = 0U; i < list.count; i++) {
            char *child = kmalloc(SHELL_PATH_MAX, GFP_KERNEL);
            if (child == NULL) {
                rc = -ENOMEM;
                break;
            }
            snprintf(child, SHELL_PATH_MAX, "%s/%s", norm, list.items[i].name);
            rc = remove_recursive_depth(shell, child, force, verbose, depth + 1);
            kfree(child);
            if (rc != 0) {
                break;
            }
        }
        if (list.items) {
            kfree(list.items);
        }
        if (rc != 0) {
            return rc;
        }
        rc = nshell_resolve_parent(shell, norm, &parent, name, sizeof(name), NULL, 0U);
        if (rc != 0) {
            return rc;
        }
        rc = vfs_remove_entry(parent, name, true);
        if (rc == 0 && verbose) {
            shell_printf("removed directory '%s'\n", norm);
        }
        return rc;
    }

    rc = nshell_resolve_parent(shell, norm, &parent, name, sizeof(name), NULL, 0U);
    if (rc != 0) {
        return rc;
    }
    rc = vfs_remove_entry(parent, name, false);
    if (rc == 0 && verbose) {
        shell_printf("removed '%s'\n", norm);
    }
    return rc;
}

int nshell_remove_recursive(struct nshell *shell, const char *path,
                            bool force, bool verbose)
{
    return remove_recursive_depth(shell, path, force, verbose, 0);
}

bool nshell_glob_match(const char *pattern, const char *text)
{
    if (pattern == NULL || text == NULL) {
        return false;
    }
    while (*pattern != '\0') {
        if (*pattern == '*') {
            while (*pattern == '*') {
                pattern++;
            }
            if (*pattern == '\0') {
                return true;
            }
            while (*text != '\0') {
                if (nshell_glob_match(pattern, text)) {
                    return true;
                }
                text++;
            }
            return false;
        }
        if (*pattern == '?') {
            if (*text == '\0') {
                return false;
            }
            pattern++;
            text++;
            continue;
        }
        if (*pattern == '[') {
            bool negate = false;
            bool matched = false;
            char ch = *text;
            if (ch == '\0') {
                return false;
            }
            pattern++;
            if (*pattern == '!' || *pattern == '^') {
                negate = true;
                pattern++;
            }
            while (*pattern != '\0' && *pattern != ']') {
                if (*(pattern + 1) == '-' && *(pattern + 2) != '\0' &&
                    *(pattern + 2) != ']') {
                    if (ch >= *pattern && ch <= *(pattern + 2)) {
                        matched = true;
                    }
                    pattern += 3;
                } else {
                    if (ch == *pattern) {
                        matched = true;
                    }
                    pattern++;
                }
            }
            if (*pattern == ']') {
                pattern++;
            }
            if (negate ? matched : !matched) {
                return false;
            }
            text++;
            continue;
        }
        if (*pattern == '\\' && *(pattern + 1) != '\0') {
            pattern++;
        }
        if (*pattern != *text) {
            return false;
        }
        pattern++;
        text++;
    }
    return *text == '\0';
}

void nshell_format_mode(umode_t mode, char out[11])
{
    out[0] = S_ISDIR(mode) ? 'd' :
             S_ISCHR(mode) ? 'c' :
             S_ISBLK(mode) ? 'b' :
             S_ISLNK(mode) ? 'l' :
             S_ISFIFO(mode) ? 'p' : '-';
    out[1] = (mode & 0400) ? 'r' : '-';
    out[2] = (mode & 0200) ? 'w' : '-';
    out[3] = (mode & 0100) ? 'x' : '-';
    out[4] = (mode & 0040) ? 'r' : '-';
    out[5] = (mode & 0020) ? 'w' : '-';
    out[6] = (mode & 0010) ? 'x' : '-';
    out[7] = (mode & 0004) ? 'r' : '-';
    out[8] = (mode & 0002) ? 'w' : '-';
    out[9] = (mode & 0001) ? 'x' : '-';
    out[10] = '\0';
}

void nshell_format_size(unsigned long long bytes, bool human,
                        char *out, size_t out_size)
{
    static const char units[] = { 'B', 'K', 'M', 'G' };
    unsigned int unit = 0U;
    unsigned long long whole = bytes;
    unsigned int tenth = 0U;

    if (!human || bytes < 1024ULL) {
        snprintf(out, out_size, "%llu", bytes);
        return;
    }
    while (whole >= 1024ULL && unit < 3U) {
        tenth = (unsigned int)(((whole % 1024ULL) * 10ULL) / 1024ULL);
        whole /= 1024ULL;
        unit++;
    }
    if (whole < 10ULL) {
        snprintf(out, out_size, "%llu.%u%c", whole, tenth, units[unit]);
    } else {
        snprintf(out, out_size, "%llu%c", whole, units[unit]);
    }
}
