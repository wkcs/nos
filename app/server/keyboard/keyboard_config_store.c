/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Named keyboard configuration files stored on a caller-selected filesystem.
 */
#include <nos/keyboard_config.h>
#include <nos/nlk_bearer.h>
#include <kernel/mm.h>
#include <uapi/nos/errno.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <lib/vsprintf.h>
#include <string.h>

#define CONFIG_PATH_SIZE 128U

static const char *config_subdir(uint8_t kind)
{
    if (kind == KEYBOARD_CONFIG_KEYBOARD) return "config";
    if (kind == KEYBOARD_CONFIG_LIGHT) return "light";
    if (kind == KEYBOARD_CONFIG_MACRO) return "macro";
    return NULL;
}

static int config_valid_id(const char *id)
{
    size_t i;
    if (id == NULL || id[0] == '\0') return 0;
    for (i = 0U; i < KEYBOARD_CONFIG_ID_SIZE; i++) {
        char c = id[i];
        if (c == '\0') return 1;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
    }
    return 0;
}

static int config_ensure_dirs(const char *dir)
{
    char sub[CONFIG_PATH_SIZE];
    static const uint8_t kinds[3] = {
        KEYBOARD_CONFIG_KEYBOARD,
        KEYBOARD_CONFIG_LIGHT,
        KEYBOARD_CONFIG_MACRO,
    };
    int i, n, rc;

    if (dir == NULL) return -EINVAL;
    rc = nos_mkdir(dir, 0755);
    if (rc != 0 && rc != -EEXIST) return rc;
    for (i = 0; i < 3; i++) {
        n = snprintf(sub, sizeof(sub), "%s/%s", dir, config_subdir(kinds[i]));
        if (n < 0 || (size_t)n >= sizeof(sub)) return -EINVAL;
        rc = nos_mkdir(sub, 0755);
        if (rc != 0 && rc != -EEXIST) return rc;
    }
    return 0;
}

static int config_path(char *path, const char *dir, const char *id, uint8_t kind, int temp)
{
    int n;
    const char *sub;
    if (path == NULL || dir == NULL) return -EINVAL;
    if (kind == KEYBOARD_CONFIG_ACTIVE) {
        n = snprintf(path, CONFIG_PATH_SIZE, temp ? "%s/active.tmp" : "%s/active.nkc", dir);
        return n < 0 || (size_t)n >= CONFIG_PATH_SIZE ? -EINVAL : 0;
    }
    sub = config_subdir(kind);
    if (sub == NULL || !config_valid_id(id)) return -EINVAL;
    n = snprintf(path, CONFIG_PATH_SIZE, temp ? "%s/%s/t-%s.tmp" : "%s/%s/%s.nkc",
                 dir, sub, id);
    return n < 0 || (size_t)n >= CONFIG_PATH_SIZE ? -EINVAL : 0;
}

static size_t config_max_size(const struct nlk_keyboard_ctx *ctx)
{
    size_t cells = (size_t)ctx->desc.key_count * ctx->desc.layer_count;
    return sizeof(struct keyboard_config_header) + KEYBOARD_CONFIG_ID_SIZE * 2U +
           sizeof(struct nlk_kb_params) +
           sizeof(struct nlk_kb_analog_cfg) +
           cells * (2U + NLK_KB_MAX_KEY_ACTIONS * NLK_KB_ACTION_NAME_MAX) + 1U +
           NLK_KB_MAX_MACROS * (1U + sizeof(struct nlk_kb_macro_info) +
               NLK_KB_MAX_MACRO_STEPS * sizeof(struct nlk_kb_macro_step)) +
           sizeof(struct nlk_kb_light_cfg) + NLK_KB_LIGHT_EFFECT_NAME_MAX + 1U +
           NLK_KB_MAX_LIGHT_ZONES * (sizeof(struct nlk_kb_zone_light_cfg) +
                                    NLK_KB_LIGHT_EFFECT_NAME_MAX);
}

static int config_write_file(const struct nlk_keyboard_ctx *ctx, const char *hardware,
                             const char *dir, const char *id, const char *name,
                             uint8_t kind, uint8_t macro_id, int replace)
{
    char path[CONFIG_PATH_SIZE], temp[CONFIG_PATH_SIZE];
    void *buffer;
    size_t length;
    ssize_t count;
    int fd, ret, close_ret;

    ret = config_ensure_dirs(dir);
    if (ret != 0) return ret;
    if (config_path(path, dir, id, kind, 0) != 0 ||
        config_path(temp, dir, id, kind, 1) != 0) return -EINVAL;
    if (!replace && vfs_lookup_path(vfs_get_root(), path) != NULL) return -EEXIST;
    ret = keyboard_config_encode(ctx, hardware, name, kind, macro_id, NULL, 0U, &length);
    if (ret != 0) return ret;
    buffer = kmalloc(length, GFP_KERNEL);
    if (buffer == NULL) return -ENOMEM;
    ret = keyboard_config_encode(ctx, hardware, name, kind, macro_id, buffer, length, &length);
    if (ret != 0) goto out;
    fd = nos_open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { ret = fd; goto out; }
    count = nos_write(fd, buffer, length);
    close_ret = nos_close(fd);
    if (count != (ssize_t)length || close_ret != 0) {
        ret = count < 0 ? (int)count : -EIO;
        (void)nos_unlink(temp);
        goto out;
    }
    if (replace) {
        (void)nos_unlink(path);
    }
    ret = nos_rename(temp, path);
    if (ret != 0) (void)nos_unlink(temp);
out:
    kfree(buffer);
    return ret;
}

int keyboard_config_file_save(const struct nlk_keyboard_ctx *ctx, const char *hardware,
                              const char *dir, const char *id, const char *name,
                              uint8_t kind, uint8_t macro_id)
{
    if (kind == KEYBOARD_CONFIG_ACTIVE) return -EINVAL;
    return config_write_file(ctx, hardware, dir, id, name, kind, macro_id, 0);
}

int keyboard_config_active_save(const struct nlk_keyboard_ctx *ctx,
                                const char *hardware, const char *dir)
{
    return config_write_file(ctx, hardware, dir, NULL, "active",
                             KEYBOARD_CONFIG_ACTIVE, 0U, 1);
}

int keyboard_config_file_load(struct nlk_keyboard_ctx *ctx, const char *hardware,
                              const char *dir, const char *id, uint8_t kind,
                              uint8_t macro_id)
{
    char path[CONFIG_PATH_SIZE];
    struct keyboard_config_header header;
    void *buffer;
    size_t length;
    ssize_t count;
    loff_t file_size;
    int fd, ret;

    if (ctx == NULL || config_path(path, dir, id, kind, 0) != 0) return -EINVAL;
    fd = nos_open(path, O_RDONLY, 0);
    if (fd < 0) return fd;
    count = nos_read(fd, &header, sizeof(header));
    file_size = nos_lseek(fd, 0, SEEK_END);
    if (count != (ssize_t)sizeof(header) || header.length < sizeof(header) ||
        header.length > config_max_size(ctx) || file_size != (loff_t)header.length ||
        nos_lseek(fd, 0, SEEK_SET) < 0) {
        (void)nos_close(fd);
        return -EINVAL;
    }
    length = header.length;
    buffer = kmalloc(length, GFP_KERNEL);
    if (buffer == NULL) { (void)nos_close(fd); return -ENOMEM; }
    count = nos_read(fd, buffer, length);
    ret = nos_close(fd);
    if (count != (ssize_t)length || ret != 0) ret = -EIO;
    else ret = keyboard_config_decode(ctx, hardware, kind, macro_id, buffer, length);
    kfree(buffer);
    return ret;
}

struct config_dir_cursor {
    uint32_t wanted;
    uint32_t seen;
    char id[KEYBOARD_CONFIG_ID_SIZE];
    int found;
};

static int config_filldir(void *arg, const char *name, int len, loff_t offset,
                          u64 ino, unsigned type)
{
    struct config_dir_cursor *scan = arg;
    size_t id_len;
    (void)offset; (void)ino;
    if (scan->found || name == NULL || len < 5 || type == DT_DIR ||
        memcmp(name + len - 4, ".nkc", 4U) != 0) return 0;
    id_len = (size_t)len - 4U;
    if (id_len >= sizeof(scan->id)) return 0;
    memcpy(scan->id, name, id_len);
    scan->id[id_len] = '\0';
    if (!config_valid_id(scan->id)) return 0;
    if (scan->seen++ != scan->wanted) return 0;
    scan->found = 1;
    return 1;
}

int keyboard_config_file_next(const char *dir, const char *hardware, uint8_t kind,
                              uint32_t *cursor, struct keyboard_config_file_info *info)
{
    struct dentry *dentry;
    struct file file;
    struct config_dir_cursor scan;
    struct keyboard_config_header header;
    char sub_dir[CONFIG_PATH_SIZE];
    char path[CONFIG_PATH_SIZE];
    const char *sub = config_subdir(kind);
    int fd, n, ret;

    if (dir == NULL || hardware == NULL || cursor == NULL || info == NULL ||
        sub == NULL) return -EINVAL;
    dentry = vfs_lookup_path(vfs_get_root(), dir);
    if (dentry == NULL || dentry->d_inode == NULL) return -ENOENT;
    if (!S_ISDIR(dentry->d_inode->i_mode)) return -ENOTDIR;
    n = snprintf(sub_dir, sizeof(sub_dir), "%s/%s", dir, sub);
    if (n < 0 || (size_t)n >= sizeof(sub_dir)) return -EINVAL;
    dentry = vfs_lookup_path(vfs_get_root(), sub_dir);
    if (dentry == NULL || dentry->d_inode == NULL) return 0;
    if (!S_ISDIR(dentry->d_inode->i_mode)) return -ENOTDIR;
    if (dentry->d_inode->i_fop == NULL || dentry->d_inode->i_fop->readdir == NULL) return -ENOTSUP;
    for (;;) {
        memset(&file, 0, sizeof(file));
        file.f_inode = dentry->d_inode;
        file.f_op = file.f_inode->i_fop;
        file.f_path.dentry = dentry;
        if (file.f_op->open != NULL) {
            ret = file.f_op->open(file.f_inode, &file);
            if (ret != 0) return ret;
        }
        memset(&scan, 0, sizeof(scan));
        scan.wanted = *cursor;
        ret = file.f_op->readdir(&file, &scan, config_filldir);
        if (file.f_op->release != NULL) (void)file.f_op->release(file.f_inode, &file);
        if (ret < 0) return ret;
        if (!scan.found) return 0;
        (*cursor)++;
        if (config_path(path, dir, scan.id, kind, 0) != 0) continue;
        fd = nos_open(path, O_RDONLY, 0);
        if (fd < 0) continue;
        ret = (int)nos_read(fd, &header, sizeof(header));
        (void)nos_close(fd);
        if (ret != (int)sizeof(header) || header.magic != KEYBOARD_CONFIG_MAGIC ||
            header.version != 1U || header.kind != kind ||
            memchr(header.hardware, '\0', sizeof(header.hardware)) == NULL ||
            memchr(header.name, '\0', sizeof(header.name)) == NULL ||
            strcmp(header.hardware, hardware) != 0) continue;
        memset(info, 0, sizeof(*info));
        memcpy(info->id, scan.id, strlen(scan.id) + 1U);
        memcpy(info->name, header.name, sizeof(info->name));
        info->size = header.length;
        return 1;
    }
}

int keyboard_config_file_delete(const char *dir, const char *id, uint8_t kind)
{
    char path[CONFIG_PATH_SIZE];
    if (kind == KEYBOARD_CONFIG_ACTIVE || config_path(path, dir, id, kind, 0) != 0) return -EINVAL;
    return nos_unlink(path);
}

int keyboard_config_file_restore(struct nlk_keyboard_ctx *ctx, const char *hardware,
                                 const char *dir, uint8_t kind)
{
    if (kind != KEYBOARD_CONFIG_KEYBOARD && kind != KEYBOARD_CONFIG_LIGHT &&
        kind != KEYBOARD_CONFIG_ACTIVE) return -EINVAL;
    return keyboard_config_file_load(ctx, hardware, dir, NULL, KEYBOARD_CONFIG_ACTIVE, 0U);
}

int keyboard_config_file_rpc(struct nlk_keyboard_ctx *ctx, const char *hardware,
                             const char *dir, uint8_t op, const void *req,
                             uint16_t req_len, void *resp, uint16_t *resp_len)
{
    const struct nlk_kb_config_file_req *file_req = req;
    const struct nlk_kb_config_list_req *list_req = req;
    struct nlk_kb_config_files_info *details = resp;
    struct nlk_kb_config_list_resp *page = resp;
    struct keyboard_config_file_info info;
    uint32_t cursor;
    int ret;

    if (ctx == NULL || hardware == NULL || dir == NULL || resp_len == NULL ||
        memchr(hardware, '\0', NLK_KB_CONFIG_HARDWARE_SIZE) == NULL) return -EINVAL;
    *resp_len = 0U;
    if (op == 0U) {
        return keyboard_config_active_save(ctx, hardware, dir);
    }
    if (op == NLK_KB_OP_CONFIG_FILES_INFO) {
        if (resp == NULL) return -EINVAL;
        memset(details, 0, sizeof(*details));
        details->version = 1U;
        details->key_count = ctx->desc.key_count;
        details->layer_count = ctx->desc.layer_count;
        details->macro_slots = NLK_KB_MAX_MACROS;
        details->macro_steps = NLK_KB_MAX_MACRO_STEPS;
        details->light_zones = ctx->light_zone_count;
        memcpy(details->hardware, hardware, strlen(hardware) + 1U);
        memcpy(details->active_keyboard, ctx->active_keyboard_id, sizeof(details->active_keyboard));
        memcpy(details->active_light, ctx->active_light_id, sizeof(details->active_light));
        *resp_len = sizeof(*details);
        return 0;
    }
    if (op == NLK_KB_OP_CONFIG_FILES_LIST) {
        if (req == NULL || req_len < sizeof(*list_req) || resp == NULL) return -EINVAL;
        cursor = list_req->cursor;
        ret = keyboard_config_file_next(dir, hardware, list_req->kind, &cursor, &info);
        if (ret < 0) return ret;
        memset(page, 0, sizeof(*page));
        page->next_cursor = cursor;
        page->eof = (ret == 0);
        if (ret != 0) {
            page->size = info.size;
            memcpy(page->id, info.id, sizeof(page->id));
            memcpy(page->name, info.name, sizeof(page->name));
        }
        *resp_len = sizeof(*page);
        return 0;
    }
    if (req == NULL || req_len < sizeof(*file_req) ||
        memchr(file_req->id, '\0', sizeof(file_req->id)) == NULL) return -EINVAL;
    if (op == NLK_KB_OP_CONFIG_FILES_SAVE) {
        if (memchr(file_req->name, '\0', sizeof(file_req->name)) == NULL) return -EINVAL;
        ret = keyboard_config_file_save(ctx, hardware, dir, file_req->id, file_req->name,
                                        file_req->kind, file_req->macro_id);
        if (ret == 0) {
            if (file_req->kind == KEYBOARD_CONFIG_KEYBOARD) {
                memcpy(ctx->active_keyboard_id, file_req->id, sizeof(ctx->active_keyboard_id));
                ret = keyboard_config_active_save(ctx, hardware, dir);
            } else if (file_req->kind == KEYBOARD_CONFIG_LIGHT) {
                memcpy(ctx->active_light_id, file_req->id, sizeof(ctx->active_light_id));
                ret = keyboard_config_active_save(ctx, hardware, dir);
            }
        }
        return ret;
    }
    if (op == NLK_KB_OP_CONFIG_FILES_LOAD) {
        void *backup = NULL;
        size_t backup_len = 0U;
        ret = keyboard_config_encode(ctx, hardware, "active", KEYBOARD_CONFIG_ACTIVE,
                                     0U, NULL, 0U, &backup_len);
        if (ret != 0) return ret;
        backup = kmalloc(backup_len, GFP_KERNEL);
        if (backup == NULL) return -ENOMEM;
        ret = keyboard_config_encode(ctx, hardware, "active", KEYBOARD_CONFIG_ACTIVE,
                                     0U, backup, backup_len, &backup_len);
        if (ret != 0) { kfree(backup); return ret; }
        ret = keyboard_config_file_load(ctx, hardware, dir, file_req->id,
                                        file_req->kind, file_req->macro_id);
        if (ret == 0) {
            if (file_req->kind == KEYBOARD_CONFIG_KEYBOARD) {
                memcpy(ctx->active_keyboard_id, file_req->id, sizeof(ctx->active_keyboard_id));
            } else if (file_req->kind == KEYBOARD_CONFIG_LIGHT) {
                memcpy(ctx->active_light_id, file_req->id, sizeof(ctx->active_light_id));
            }
            ret = keyboard_config_active_save(ctx, hardware, dir);
            if (ret != 0) {
                (void)keyboard_config_decode(ctx, hardware, KEYBOARD_CONFIG_ACTIVE,
                                             0U, backup, backup_len);
            }
        }
        kfree(backup);
        return ret;
    }
    if (op == NLK_KB_OP_CONFIG_FILES_DELETE) {
        if ((file_req->kind == KEYBOARD_CONFIG_KEYBOARD &&
             strcmp(ctx->active_keyboard_id, file_req->id) == 0) ||
            (file_req->kind == KEYBOARD_CONFIG_LIGHT &&
             strcmp(ctx->active_light_id, file_req->id) == 0)) {
            return -EBUSY;
        }
        return keyboard_config_file_delete(dir, file_req->id, file_req->kind);
    }
    return -ENOTSUP;
}
