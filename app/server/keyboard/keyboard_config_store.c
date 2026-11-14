/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Named keyboard configuration files stored on a caller-selected filesystem.
 */
#include <nos/keyboard_config.h>
#include <nos/nlk_bearer.h>
#include <kernel/mm.h>
#include <kernel/mutex.h>
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
           sizeof(struct nlk_kb_light_override) +
           cells * (sizeof(uint32_t) + 1U + NLK_KB_MAX_KEY_ACTIONS * NLK_KB_ACTION_NAME_MAX) + 1U +
           255U * (1U + sizeof(struct nlk_kb_macro_info) + NLK_KB_MACRO_DESC_MAX +
               NLK_KB_MAX_MACRO_STEPS * sizeof(struct nlk_kb_macro_step)) +
           sizeof(struct nlk_kb_light_cfg) + NLK_KB_LIGHT_EFFECT_NAME_MAX + 1U +
           NLK_KB_MAX_LIGHT_ZONES * (sizeof(struct nlk_kb_zone_light_cfg) +
                                    NLK_KB_LIGHT_EFFECT_NAME_MAX) + 1U +
           CONFIG_KEYBOARD_LIGHT_MAX_LAYERS *
               (sizeof(struct nlk_kb_light_layer) + NLK_KB_LIGHT_EFFECT_NAME_MAX);
}

/* Compare in small chunks: a repeated save must not erase/program Flash. */
static int config_file_matches(const char *path, const void *buffer, size_t length)
{
    uint8_t chunk[64];
    const uint8_t *data = buffer;
    size_t offset = 0U;
    int fd = nos_open(path, O_RDONLY, 0);
    int equal = 1;

    if (fd < 0) return 0;
    if (nos_lseek(fd, 0, SEEK_END) != (loff_t)length ||
        nos_lseek(fd, 0, SEEK_SET) < 0) equal = 0;
    while (equal && offset < length) {
        size_t size = length - offset;
        if (size > sizeof(chunk)) size = sizeof(chunk);
        if (nos_read(fd, chunk, size) != (ssize_t)size ||
            memcmp(chunk, data + offset, size) != 0) equal = 0;
        offset += size;
    }
    if (nos_close(fd) != 0) equal = 0;
    return equal;
}

/* Return one for a new write, zero for identical content, or a negative errno. */
static int config_write_buffer(const char *dir, const char *id, uint8_t kind,
                                const void *buffer, size_t length)
{
    char path[CONFIG_PATH_SIZE], temp[CONFIG_PATH_SIZE];
    ssize_t count;
    int fd, ret, close_ret;

    if (config_path(path, dir, id, kind, 0) != 0 ||
        config_path(temp, dir, id, kind, 1) != 0) return -EINVAL;
    if (config_file_matches(path, buffer, length)) return 0;
    ret = config_ensure_dirs(dir);
    if (ret != 0) return ret;
    fd = nos_open(temp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return fd;
    count = nos_write(fd, buffer, length);
    close_ret = nos_close(fd);
    if (count != (ssize_t)length || close_ret != 0) {
        (void)nos_unlink(temp);
        return count < 0 ? (int)count : -EIO;
    }
    ret = nos_rename(temp, path);
    if (ret == -EEXIST && nos_unlink(path) == 0) {
        ret = nos_rename(temp, path);
    }
    if (ret != 0) (void)nos_unlink(temp);
    return ret == 0 ? 1 : ret;
}

int keyboard_config_snapshot_write(const char *dir, const void *buffer, size_t length)
{
    if (buffer == NULL || length < sizeof(struct keyboard_config_header)) return -EINVAL;
    return config_write_buffer(dir, NULL, KEYBOARD_CONFIG_ACTIVE, buffer, length);
}

static int config_write_file(const struct nlk_keyboard_ctx *ctx, const char *hardware,
                             const char *dir, const char *id, const char *name,
                             const char *desc, uint8_t kind, uint8_t macro_id, int replace)
{
    char path[CONFIG_PATH_SIZE];
    void *buffer;
    size_t length;
    int ret;

    if (config_path(path, dir, id, kind, 0) != 0) return -EINVAL;
    if (!replace && vfs_lookup_path(vfs_get_root(), path) != NULL) return -EEXIST;
    ret = keyboard_config_encode(ctx, hardware, name, desc, kind, macro_id, NULL, 0U, &length);
    if (ret != 0) return ret;
    buffer = kmalloc(length, GFP_KERNEL);
    if (buffer == NULL) return -ENOMEM;
    ret = keyboard_config_encode(ctx, hardware, name, desc, kind, macro_id, buffer, length, &length);
    if (ret == 0) ret = config_write_buffer(dir, id, kind, buffer, length);
    kfree(buffer);
    return ret < 0 ? ret : 0;
}

int keyboard_config_file_save(const struct nlk_keyboard_ctx *ctx, const char *hardware,
                              const char *dir, const char *id, const char *name,
                              const char *desc, uint8_t kind, uint8_t macro_id)
{
    if (kind == KEYBOARD_CONFIG_ACTIVE) return -EINVAL;
    return config_write_file(ctx, hardware, dir, id, name, desc, kind, macro_id, 0);
}

int keyboard_config_active_save(const struct nlk_keyboard_ctx *ctx,
                                const char *hardware, const char *dir)
{
    return config_write_file(ctx, hardware, dir, NULL, "active", "",
                             KEYBOARD_CONFIG_ACTIVE, 0U, 1);
}

static int config_read_file(const struct nlk_keyboard_ctx *ctx, const char *dir,
                             const char *id, uint8_t kind, void **buffer, size_t *length)
{
    char path[CONFIG_PATH_SIZE];
    struct keyboard_config_header header;
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
    *length = header.length;
    *buffer = kmalloc(*length, GFP_KERNEL);
    if (*buffer == NULL) { (void)nos_close(fd); return -ENOMEM; }
    count = nos_read(fd, *buffer, *length);
    ret = nos_close(fd);
    if (count != (ssize_t)*length || ret != 0) {
        kfree(*buffer);
        *buffer = NULL;
        return -EIO;
    }
    return 0;
}

/* Only macro_play actions interpret the high 24 keycode bits as a macro hash. */
static int config_macro_bound(const struct nlk_keyboard_ctx *ctx, uint8_t slot,
                               uint32_t hash, uint32_t name_hash)
{
    uint8_t layer, key, action;

    for (layer = 0U; layer < ctx->desc.layer_count; layer++) {
        for (key = 0U; key < ctx->desc.key_count; key++) {
            const struct nlk_kb_key_action_chain *chain = &ctx->action_chains[layer][key];
            uint32_t code = ctx->keymap[layer][key];
            for (action = 0U; action < chain->count; action++) {
                uint8_t id = chain->action_ids[action];
                if (id == 0U || id > ctx->action_count ||
                    strcmp(ctx->action_descs[id - 1U].name, "macro_play") != 0) continue;
                if ((code >> 8) != 0U) {
                    if ((hash != 0U && (code >> 8) == hash) ||
                        (name_hash != 0U && (code >> 8) == name_hash)) return 1;
                } else if (slot != NLK_KB_MACRO_NEW &&
                           code >= NLK_KB_MACRO_KEYCODE_BASE &&
                           code <= 0xFFU &&
                           code - NLK_KB_MACRO_KEYCODE_BASE == slot) {
                    return 1;
                }
            }
        }
    }
    return 0;
}

int keyboard_config_activate_macros(struct nlk_keyboard_ctx *ctx, const char *hardware,
                                    const char *dir, struct mutex *lock)
{
    struct keyboard_config_file_info info;
    struct nlk_kb_macro **link;
    uint32_t cursor = 0U;
    int ret;

    if (ctx == NULL || hardware == NULL || dir == NULL) return -EINVAL;
    /* Read only headers while enumerating. Unreferenced macro steps stay on disk. */
    while ((ret = keyboard_config_file_next(dir, hardware, KEYBOARD_CONFIG_MACRO,
                                             &cursor, &info)) > 0) {
        uint32_t hash = nlk_fnv1a_24(info.id);
        void *buffer = NULL;
        size_t length = 0U;
        int needed;

        if (lock != NULL) mutex_lock(lock);
        needed = config_macro_bound(ctx, NLK_KB_MACRO_NEW, hash, nlk_fnv1a_24(info.name));
        if (lock != NULL) mutex_unlock(lock);
        if (!needed) continue;
        ret = config_read_file(ctx, dir, info.id, KEYBOARD_CONFIG_MACRO, &buffer, &length);
        if (ret != 0) return ret;
        if (lock != NULL) mutex_lock(lock);
        /* A key-triggered profile switch may have happened during file I/O. */
        if (config_macro_bound(ctx, NLK_KB_MACRO_NEW, hash, nlk_fnv1a_24(info.name))) {
            struct nlk_kb_macro *macro;
            uint8_t slot = NLK_KB_MACRO_NEW;
            for (macro = ctx->macros; macro != NULL; macro = macro->next) {
                if (strcmp(macro->info.id, info.id) == 0) {
                    slot = macro->info.macro_id;
                    break;
                }
            }
            ret = keyboard_config_decode(ctx, hardware, KEYBOARD_CONFIG_MACRO, slot, buffer, length);
        }
        if (lock != NULL) mutex_unlock(lock);
        kfree(buffer);
        if (ret < 0) return ret;
    }
    if (ret < 0) return ret;
    if (lock != NULL) mutex_lock(lock);
    link = &ctx->macros;
    while (*link != NULL) {
        struct nlk_kb_macro *macro = *link;
        uint32_t hash = macro->info.id[0] != '\0' ? nlk_fnv1a_24(macro->info.id) : 0U;
        uint32_t name_hash = macro->info.name[0] != '\0' ? nlk_fnv1a_24(macro->info.name) : 0U;
        if (config_macro_bound(ctx, macro->info.macro_id, hash, name_hash)) {
            link = &macro->next;
        } else {
            *link = macro->next;
            macro->next = NULL;
            nlk_cap_keyboard_free_macros(macro);
            ctx->macro_count--;
        }
    }
    if (lock != NULL) mutex_unlock(lock);
    return 0;
}

int keyboard_config_file_load(struct nlk_keyboard_ctx *ctx, const char *hardware,
                              const char *dir, const char *id, uint8_t kind,
                              uint8_t macro_id)
{
    void *buffer = NULL;
    size_t length = 0U;
    int ret = config_read_file(ctx, dir, id, kind, &buffer, &length);

    if (ret != 0) return ret;
    ret = keyboard_config_decode(ctx, hardware, kind, macro_id, buffer, length);
    kfree(buffer);
    if (ret == 0 && (kind == KEYBOARD_CONFIG_KEYBOARD || kind == KEYBOARD_CONFIG_ACTIVE)) {
        ret = keyboard_config_activate_macros(ctx, hardware, dir, NULL);
    }
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

/* These buffers stay live across VFS -> LittleFS -> SPI calls. Keep them
 * off the small RPC task stack and never reserve them between requests. */
struct config_file_workspace {
    struct file file;
    struct config_dir_cursor scan;
    struct keyboard_config_header header;
    char sub_dir[CONFIG_PATH_SIZE];
    char path[CONFIG_PATH_SIZE];
};

static int config_file_next(const char *dir, const char *hardware, uint8_t kind,
                            uint32_t *cursor, struct keyboard_config_file_info *info,
                            struct config_file_workspace *work)
{
    struct dentry *dentry;
    const char *sub = config_subdir(kind);
    int fd, n, ret;

    if (dir == NULL || hardware == NULL || cursor == NULL || info == NULL ||
        sub == NULL) return -EINVAL;
    dentry = vfs_lookup_path(vfs_get_root(), dir);
    if (dentry == NULL || dentry->d_inode == NULL) return -ENOENT;
    if (!S_ISDIR(dentry->d_inode->i_mode)) return -ENOTDIR;
    n = snprintf(work->sub_dir, sizeof(work->sub_dir), "%s/%s", dir, sub);
    if (n < 0 || (size_t)n >= sizeof(work->sub_dir)) return -EINVAL;
    dentry = vfs_lookup_path(vfs_get_root(), work->sub_dir);
    if (dentry == NULL || dentry->d_inode == NULL) return 0;
    if (!S_ISDIR(dentry->d_inode->i_mode)) return -ENOTDIR;
    if (dentry->d_inode->i_fop == NULL || dentry->d_inode->i_fop->readdir == NULL) return -ENOTSUP;
    for (;;) {
        memset(&work->file, 0, sizeof(work->file));
        work->file.f_inode = dentry->d_inode;
        work->file.f_op = work->file.f_inode->i_fop;
        work->file.f_path.dentry = dentry;
        if (work->file.f_op->open != NULL) {
            ret = work->file.f_op->open(work->file.f_inode, &work->file);
            if (ret != 0) return ret;
        }
        memset(&work->scan, 0, sizeof(work->scan));
        work->scan.wanted = *cursor;
        ret = work->file.f_op->readdir(&work->file, &work->scan, config_filldir);
        if (work->file.f_op->release != NULL) (void)work->file.f_op->release(work->file.f_inode, &work->file);
        if (ret < 0) return ret;
        if (!work->scan.found) return 0;
        (*cursor)++;
        if (config_path(work->path, dir, work->scan.id, kind, 0) != 0) continue;
        fd = nos_open(work->path, O_RDONLY, 0);
        if (fd < 0) continue;
        ret = (int)nos_read(fd, &work->header, sizeof(work->header));
        (void)nos_close(fd);
        if (ret != (int)sizeof(work->header) || work->header.magic != KEYBOARD_CONFIG_MAGIC ||
            (work->header.version < 1U || work->header.version > 5U) || work->header.kind != kind ||
            memchr(work->header.hardware, '\0', sizeof(work->header.hardware)) == NULL ||
            memchr(work->header.name, '\0', sizeof(work->header.name)) == NULL ||
            (work->header.version >= 2U && memchr(work->header.desc, '\0', sizeof(work->header.desc)) == NULL) ||
            strcmp(work->header.hardware, hardware) != 0) continue;
        memset(info, 0, sizeof(*info));
        memcpy(info->id, work->scan.id, strlen(work->scan.id) + 1U);
        memcpy(info->name, work->header.name, sizeof(info->name));
        if (work->header.version >= 2U) {
            memcpy(info->desc, work->header.desc, sizeof(info->desc));
        }
        info->size = work->header.length;
        return 1;
    }
}

int keyboard_config_file_next(const char *dir, const char *hardware, uint8_t kind,
                              uint32_t *cursor, struct keyboard_config_file_info *info)
{
    struct config_file_workspace *work;
    int ret;

    if (dir == NULL || hardware == NULL || cursor == NULL || info == NULL ||
        config_subdir(kind) == NULL) return -EINVAL;
    work = kmalloc(sizeof(*work), GFP_KERNEL);
    if (work == NULL) return -ENOMEM;
    ret = config_file_next(dir, hardware, kind, cursor, info, work);
    kfree(work);
    return ret;
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
        details->macro_slots = 0U; /* Macros are allocated on demand. */
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
            memcpy(page->desc, info.desc, sizeof(page->desc));
        }
        *resp_len = sizeof(*page);
        return 0;
    }
    if (req == NULL || req_len < sizeof(*file_req) ||
        memchr(file_req->id, '\0', sizeof(file_req->id)) == NULL) return -EINVAL;
    if (op == NLK_KB_OP_CONFIG_FILES_SAVE) {
        if (memchr(file_req->name, '\0', sizeof(file_req->name)) == NULL) return -EINVAL;
        if (memchr(file_req->desc, '\0', sizeof(file_req->desc)) == NULL) return -EINVAL;
        ret = config_write_file(ctx, hardware, dir, file_req->id, file_req->name,
                                 file_req->desc, file_req->kind, file_req->macro_id,
                                 file_req->kind == KEYBOARD_CONFIG_MACRO);
        if (ret == 0) {
            (void)keyboard_config_manifest_update((struct nlk_kb_manifest *)&ctx->manifest,
                                                  file_req->kind, file_req->id, 0);
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
        ret = keyboard_config_encode(ctx, hardware, "active", "", KEYBOARD_CONFIG_ACTIVE,
                                     0U, NULL, 0U, &backup_len);
        if (ret != 0) return ret;
        backup = kmalloc(backup_len, GFP_KERNEL);
        if (backup == NULL) return -ENOMEM;
        ret = keyboard_config_encode(ctx, hardware, "active", "", KEYBOARD_CONFIG_ACTIVE,
                                     0U, backup, backup_len, &backup_len);
        if (ret != 0) { kfree(backup); return ret; }
        ret = keyboard_config_file_load(ctx, hardware, dir, file_req->id,
                                        file_req->kind, file_req->macro_id);
        if (ret == 0) {
            if (file_req->kind == KEYBOARD_CONFIG_KEYBOARD) {
                memcpy(ctx->active_keyboard_id, file_req->id, sizeof(ctx->active_keyboard_id));
                if (ctx->active_light_override.light_id[0] != '\0') {
                    (void)keyboard_config_file_load(ctx, hardware, dir,
                                                    ctx->active_light_override.light_id,
                                                    KEYBOARD_CONFIG_LIGHT, 0U);
                    memcpy(ctx->active_light_id, ctx->active_light_override.light_id,
                           sizeof(ctx->active_light_id));
                    if ((ctx->active_light_override.override_flags & NLK_KB_LIGHT_OVERRIDE_BRIGHTNESS) != 0U) {
                        ctx->light_cfg.brightness = ctx->active_light_override.brightness;
                    }
                    if ((ctx->active_light_override.override_flags & NLK_KB_LIGHT_OVERRIDE_SPEED) != 0U) {
                        ctx->light_cfg.speed = ctx->active_light_override.speed;
                    }
                    if ((ctx->active_light_override.override_flags & NLK_KB_LIGHT_OVERRIDE_COLOR) != 0U) {
                        ctx->light_cfg.color_r = ctx->active_light_override.color_r;
                        ctx->light_cfg.color_g = ctx->active_light_override.color_g;
                        ctx->light_cfg.color_b = ctx->active_light_override.color_b;
                    }
                    if (ctx->light_cb != NULL) {
                        ctx->light_cb(&ctx->light_cfg, NULL, ctx->indicator_leds, ctx->light_priv);
                    }
                }
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
        ret = keyboard_config_file_delete(dir, file_req->id, file_req->kind);
        if (ret == 0) {
            (void)keyboard_config_manifest_update((struct nlk_kb_manifest *)&ctx->manifest,
                                                  file_req->kind, file_req->id, 1);
        }
        return ret;
    }
    return -ENOTSUP;
}

int keyboard_config_manifest_build(const char *dir, const char *hardware, struct nlk_kb_manifest *manifest)
{
    uint32_t cursor = 0U;
    struct keyboard_config_file_info info;

    if (dir == NULL || manifest == NULL) {
        return -EINVAL;
    }
    memset(manifest, 0, sizeof(*manifest));

    /* 扫描键盘配置文件 */
    cursor = 0U;
    while (manifest->config_count < NLK_KB_MANIFEST_MAX_CONFIGS &&
           keyboard_config_file_next(dir, hardware, KEYBOARD_CONFIG_KEYBOARD, &cursor, &info) > 0) {
        struct nlk_kb_manifest_entry *ent = &manifest->configs[manifest->config_count++];
        ent->hash = nlk_fnv1a_24(info.id);
        memcpy(ent->id, info.id, sizeof(ent->id));
    }

    /* 扫描宏配置文件 */
    cursor = 0U;
    while (manifest->macro_count < NLK_KB_MANIFEST_MAX_MACROS &&
           keyboard_config_file_next(dir, hardware, KEYBOARD_CONFIG_MACRO, &cursor, &info) > 0) {
        struct nlk_kb_manifest_entry *ent = &manifest->macros[manifest->macro_count++];
        ent->hash = nlk_fnv1a_24(info.id);
        memcpy(ent->id, info.id, sizeof(ent->id));
    }

    return 0;
}

const char *keyboard_config_manifest_lookup_config(const struct nlk_kb_manifest *manifest, uint32_t hash)
{
    uint8_t i;
    if (manifest == NULL || hash == 0U) {
        return NULL;
    }
    for (i = 0U; i < manifest->config_count && i < NLK_KB_MANIFEST_MAX_CONFIGS; i++) {
        if (manifest->configs[i].hash == hash) {
            return manifest->configs[i].id;
        }
    }
    return NULL;
}

const char *keyboard_config_manifest_lookup_macro(const struct nlk_kb_manifest *manifest, uint32_t hash)
{
    uint8_t i;
    if (manifest == NULL || hash == 0U) {
        return NULL;
    }
    for (i = 0U; i < manifest->macro_count && i < NLK_KB_MANIFEST_MAX_MACROS; i++) {
        if (manifest->macros[i].hash == hash) {
            return manifest->macros[i].id;
        }
    }
    return NULL;
}

int keyboard_config_manifest_update(struct nlk_kb_manifest *manifest, uint8_t kind, const char *id, int remove)
{
    uint8_t i;
    uint32_t hash;
    if (manifest == NULL || id == NULL || id[0] == '\0') {
        return -EINVAL;
    }
    hash = nlk_fnv1a_24(id);
    if (kind == KEYBOARD_CONFIG_KEYBOARD) {
        for (i = 0U; i < manifest->config_count; i++) {
            if (manifest->configs[i].hash == hash && strcmp(manifest->configs[i].id, id) == 0) {
                if (remove) {
                    for (; i + 1U < manifest->config_count; i++) {
                        manifest->configs[i] = manifest->configs[i + 1U];
                    }
                    manifest->config_count--;
                }
                return 0;
            }
        }
        if (!remove && manifest->config_count < NLK_KB_MANIFEST_MAX_CONFIGS) {
            struct nlk_kb_manifest_entry *ent = &manifest->configs[manifest->config_count++];
            ent->hash = hash;
            memcpy(ent->id, id, sizeof(ent->id));
            return 0;
        }
    } else if (kind == KEYBOARD_CONFIG_MACRO) {
        for (i = 0U; i < manifest->macro_count; i++) {
            if (manifest->macros[i].hash == hash && strcmp(manifest->macros[i].id, id) == 0) {
                if (remove) {
                    for (; i + 1U < manifest->macro_count; i++) {
                        manifest->macros[i] = manifest->macros[i + 1U];
                    }
                    manifest->macro_count--;
                }
                return 0;
            }
        }
        if (!remove && manifest->macro_count < NLK_KB_MANIFEST_MAX_MACROS) {
            struct nlk_kb_manifest_entry *ent = &manifest->macros[manifest->macro_count++];
            ent->hash = hash;
            memcpy(ent->id, id, sizeof(ent->id));
            return 0;
        }
    }
    return 0;
}
