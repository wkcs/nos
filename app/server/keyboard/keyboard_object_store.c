/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Author: Nick Hu
 *
 * 键盘配置扁平对象存储：每个配置以 "<name>.nkc" 直接存放在 base/、light/、macro/ 目录下，
 * 聚合根记录 "/data/keyboard/config.nkc" 维护当前选中名称、全局灯光开关与单调序号。
 * 格式错误或 CRC 校验失败一律 fail closed，绝不自动格式化或静默覆盖。
 */
#include <nos/keyboard_object_store.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <kernel/errno.h>
#include <string.h>

#define HEADER_SIZE 24U
#define ROOT_SIZE (2U * KB_STORE_NAME_SIZE + 1U)
#define FORMAT_VERSION 2U
#define OBJECT_MAGIC 0x334f424bU /* "KBO3" */
#define ROOT_MAGIC 0x3352424bU   /* "KBR3" */

static void put_le(uint8_t *p, uint64_t value, unsigned int size)
{
    while (size-- != 0U) {
        *p++ = (uint8_t)value;
        value >>= 8;
    }
}

static uint64_t get_le(const uint8_t *p, unsigned int size)
{
    uint64_t value = 0U;
    unsigned int i;

    for (i = 0U; i < size; i++) {
        value |= (uint64_t)p[i] << (8U * i);
    }
    return value;
}

static uint32_t crc_update(uint32_t crc, const void *data, size_t size)
{
    const uint8_t *p = data;
    unsigned int bit;

    while (size-- != 0U) {
        crc ^= *p++;
        for (bit = 0U; bit < 8U; bit++) {
            crc = (crc >> 1) ^ (0xedb88320U & (0U - (crc & 1U)));
        }
    }
    return crc;
}

/* 自包含 UTF-8 合法性校验，避免独立主机存储测试链接额外文档模块 */
static bool valid_utf8(const uint8_t *p, size_t length)
{
    size_t i = 0U;

    while (i < length) {
        uint32_t code, minimum;
        unsigned int remaining;
        uint8_t first = p[i++];

        if (first < 0x80U) {
            continue;
        }
        if (first >= 0xc2U && first <= 0xdfU) {
            remaining = 1U;
            minimum = 0x80U;
            code = first & 0x1fU;
        } else if (first >= 0xe0U && first <= 0xefU) {
            remaining = 2U;
            minimum = 0x800U;
            code = first & 0x0fU;
        } else if (first >= 0xf0U && first <= 0xf4U) {
            remaining = 3U;
            minimum = 0x10000U;
            code = first & 0x07U;
        } else {
            return false;
        }
        if (remaining > length - i) {
            return false;
        }
        while (remaining-- != 0U) {
            if ((p[i] & 0xc0U) != 0x80U) {
                return false;
            }
            code = (code << 6) | (p[i++] & 0x3fU);
        }
        if (code < minimum || code > 0x10ffffU || (code >= 0xd800U && code <= 0xdfffU)) {
            return false;
        }
    }
    return true;
}

static bool forbidden_ascii(unsigned char c)
{
    if (c < 0x20U || c == 0x7fU) {
        return true;
    }
    switch (c) {
    case '/':
    case '\\':
    case ':':
    case '*':
    case '?':
    case '"':
    case '<':
    case '>':
    case '|':
        return true;
    default:
        return false;
    }
}

bool kb_store_valid_name(const char *name)
{
    size_t len;
    size_t i;

    if (name == NULL || name[0] == '\0') {
        return false;
    }
    len = strlen(name);
    if (len == 0U || len > KB_STORE_NAME_MAX) {
        return false;
    }
    if (name[0] == '.' || name[0] == ' ' || name[len - 1U] == '.' || name[len - 1U] == ' ' ||
        name[0] == '~') {
        return false;
    }
    for (i = 0U; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x80U && forbidden_ascii(c)) {
            return false;
        }
    }
    return valid_utf8((const uint8_t *)name, len);
}

static const char *kind_dir(uint8_t kind)
{
    if (kind == KB_STORE_BASE) {
        return KB_STORE_BASE_DIR;
    }
    if (kind == KB_STORE_LIGHT) {
        return KB_STORE_LIGHT_DIR;
    }
    if (kind == KB_STORE_MACRO) {
        return KB_STORE_MACRO_DIR;
    }
    return NULL;
}

static void object_path(char path[96], uint8_t kind, const char *name)
{
    const char *dir = kind_dir(kind);
    size_t dir_len = strlen(dir);
    size_t name_len = strlen(name);

    memcpy(path, dir, dir_len);
    path[dir_len++] = '/';
    memcpy(path + dir_len, name, name_len);
    dir_len += name_len;
    memcpy(path + dir_len, ".nkc", 5U);
}

static int mkdir_ok(const char *path)
{
    int ret = nos_mkdir(path, 0755);
    return ret == -EEXIST ? 0 : ret;
}

static int prepare_dirs(uint8_t kind)
{
    const char *dir = kind_dir(kind);
    int ret = mkdir_ok(KB_STORE_DIR);

    if (ret == 0 && dir != NULL) {
        ret = mkdir_ok(dir);
    }
    return ret;
}

static int transfer(int fd, void *buffer, size_t size, int writing)
{
    uint8_t *p = buffer;

    while (size != 0U) {
        ssize_t ret = writing ? nos_write(fd, p, size) : nos_read(fd, p, size);
        if (ret <= 0) {
            return ret < 0 ? (int)ret : -EIO;
        }
        p += ret;
        size -= (size_t)ret;
    }
    return 0;
}

static int write_record(const char *path, uint32_t magic, uint8_t kind, uint64_t revision,
                        const void *body, uint32_t size, int exclusive)
{
    uint8_t header[HEADER_SIZE] = {0};
    uint32_t crc;
    int fd, ret, close_ret;

    put_le(header, magic, 4);
    put_le(header + 4, FORMAT_VERSION, 2);
    header[6] = kind;
    put_le(header + 8, revision, 8);
    put_le(header + 16, size, 4);
    crc = crc_update(crc_update(UINT32_MAX, header, 20U), body, size) ^ UINT32_MAX;
    put_le(header + 20, crc, 4);

    fd = nos_open(path, O_WRONLY | O_CREAT | (exclusive ? O_EXCL : O_TRUNC), 0600);
    if (fd < 0) {
        return fd;
    }
    ret = transfer(fd, header, sizeof(header), 1);
    if (ret == 0) {
        ret = transfer(fd, (void *)body, size, 1);
    }
    if (ret == 0) {
        ret = nos_fsync(fd);
    }
    close_ret = nos_close(fd);
    return ret != 0 ? ret : close_ret;
}

/* 对整条记录进行边界与 CRC32 校验，防止分页读取读到损坏片段且无需整块堆缓冲 */
static int read_record(const char *path, uint32_t magic, uint8_t kind, uint64_t revision,
                       void *body, uint32_t capacity, uint32_t *size, uint64_t *out_revision,
                       int *out_fd)
{
    uint8_t header[HEADER_SIZE], scratch[128];
    uint32_t total, left, crc;
    uint64_t file_rev;
    int fd = nos_open(path, O_RDONLY), ret, close_ret;

    if (fd < 0) {
        return fd;
    }
    ret = transfer(fd, header, sizeof(header), 0);
    total = ret == 0 ? (uint32_t)get_le(header + 16, 4) : 0U;
    file_rev = ret == 0 ? get_le(header + 8, 8) : 0U;
    if (ret == 0 &&
        (get_le(header, 4) != magic || get_le(header + 4, 2) != FORMAT_VERSION ||
         header[6] != kind || header[7] != 0U || file_rev == 0U || file_rev == UINT64_MAX ||
         (revision != UINT64_MAX && file_rev != revision) || total > capacity)) {
        ret = -EINVAL;
    }
    if (ret == 0 && nos_lseek(fd, 0, SEEK_END) != (loff_t)(HEADER_SIZE + total)) {
        ret = -EINVAL;
    }
    if (ret == 0 && nos_lseek(fd, HEADER_SIZE, SEEK_SET) < 0) {
        ret = -EIO;
    }
    crc = crc_update(UINT32_MAX, header, ret == 0 ? 20U : 0U);
    left = total;
    while (ret == 0 && left != 0U) {
        uint32_t n = left < sizeof(scratch) ? left : sizeof(scratch);
        void *p = body != NULL ? (uint8_t *)body + total - left : scratch;
        ret = transfer(fd, p, n, 0);
        if (ret == 0) {
            crc = crc_update(crc, p, n);
        }
        left -= n;
    }
    if (ret == 0 && (crc ^ UINT32_MAX) != get_le(header + 20, 4)) {
        ret = -EINVAL;
    }
    if (ret == 0) {
        *size = total;
        if (out_revision != NULL) {
            *out_revision = file_rev;
        }
        if (out_fd != NULL) {
            *out_fd = fd;
            return 0;
        }
    }
    close_ret = nos_close(fd);
    return ret != 0 ? ret : close_ret;
}

const struct kb_store_entry *kb_store_find(const struct kb_store *store, uint8_t kind,
                                           const char *name)
{
    unsigned int i;

    if (store == NULL || kind_dir(kind) == NULL || !kb_store_valid_name(name)) {
        return NULL;
    }
    for (i = 0U; i < store->count; i++) {
        if (store->entries[i].kind == kind && strcmp(store->entries[i].name, name) == 0) {
            return &store->entries[i];
        }
    }
    return NULL;
}

static int valid_ref(const struct kb_store *store, uint8_t kind, const struct kb_store_ref *ref)
{
    const struct kb_store_entry *entry;

    if (ref == NULL || !kb_store_valid_name(ref->name)) {
        return 0;
    }
    entry = kb_store_find(store, kind, ref->name);
    return entry != NULL && entry->revision == ref->revision;
}

static void encode_ref_name(uint8_t *p, const struct kb_store_ref *ref)
{
    memset(p, 0, KB_STORE_NAME_SIZE);
    strcpy((char *)p, ref->name);
}

static int decode_ref_name(struct kb_store_ref *ref, const uint8_t *p)
{
    const void *nul = memchr(p, '\0', KB_STORE_NAME_SIZE);

    if (nul == NULL) {
        return -EINVAL;
    }
    memset(ref, 0, sizeof(*ref));
    strcpy(ref->name, (const char *)p);
    return kb_store_valid_name(ref->name) ? 0 : -EINVAL;
}

static int same_selection(const struct kb_store_selection *a, const struct kb_store_selection *b)
{
    return strcmp(a->base.name, b->base.name) == 0 && a->base.revision == b->base.revision &&
           strcmp(a->light.name, b->light.name) == 0 && a->light.revision == b->light.revision &&
           a->light_enabled == b->light_enabled;
}

static int root_matches(const struct kb_store *candidate)
{
    uint8_t body[ROOT_SIZE];
    uint32_t size;
    struct kb_store_ref base_ref, light_ref;
    int ret = read_record(KB_STORE_ROOT_PATH, ROOT_MAGIC, 0, candidate->sequence, body, sizeof(body),
                          &size, NULL, NULL);

    if (ret != 0) {
        return ret;
    }
    if (size != ROOT_SIZE || decode_ref_name(&base_ref, body) != 0 ||
        decode_ref_name(&light_ref, body + KB_STORE_NAME_SIZE) != 0) {
        return 0;
    }
    return strcmp(base_ref.name, candidate->selection.base.name) == 0 &&
           strcmp(light_ref.name, candidate->selection.light.name) == 0 &&
           body[2U * KB_STORE_NAME_SIZE] == (candidate->selection.light_enabled ? 1U : 0U);
}

static int object_matches(const char *path, uint8_t kind, uint64_t revision, const void *body,
                          uint16_t length)
{
    uint8_t scratch[128];
    uint32_t total, offset = 0U;
    int fd, ret, equal = 1;

    ret = read_record(path, OBJECT_MAGIC, kind, revision, NULL, KB_STORE_MAX_BODY, &total, NULL,
                      &fd);
    if (ret != 0) {
        return ret;
    }
    if (total != length || nos_lseek(fd, HEADER_SIZE, SEEK_SET) < 0) {
        equal = 0;
    }
    while (equal && offset < length) {
        size_t n = length - offset;
        if (n > sizeof(scratch)) {
            n = sizeof(scratch);
        }
        ret = transfer(fd, scratch, n, 0);
        if (ret != 0) {
            break;
        }
        if (memcmp(scratch, (const uint8_t *)body + offset, n) != 0) {
            equal = 0;
        }
        offset += (uint32_t)n;
    }
    {
        int close_ret = nos_close(fd);
        if (ret == 0) {
            ret = close_ret;
        }
    }
    return ret != 0 ? ret : equal;
}

static int publish_root(struct kb_store *store, struct kb_store *next, bool update_catalog)
{
    uint8_t body[ROOT_SIZE] = {0};
    int ret;

    if (next->sequence >= UINT64_MAX - 1U) {
        return -EOVERFLOW;
    }
    next->sequence++;
    if (update_catalog) {
        next->catalog_revision = next->sequence;
    }
    encode_ref_name(body, &next->selection.base);
    encode_ref_name(body + KB_STORE_NAME_SIZE, &next->selection.light);
    body[2U * KB_STORE_NAME_SIZE] = next->selection.light_enabled ? 1U : 0U;

    ret = write_record(KB_STORE_ROOT_TEMP, ROOT_MAGIC, 0, next->sequence, body, ROOT_SIZE, 0);
    if (ret == 0) {
        ret = nos_rename(KB_STORE_ROOT_TEMP, KB_STORE_ROOT_PATH);
        if (ret != 0 && root_matches(next) == 1) {
            /* 底层已完成替换但返回了错误时，以磁盘 CRC 校验结果为准 */
            ret = 0;
        }
    }
    if (ret == 0) {
        next->ready = true;
        *store = *next;
    }
    return ret;
}

struct dir_cursor {
    char after[32];
    char name[32];
    unsigned int type;
};

static int dir_fill(void *arg, const char *name, int length, loff_t offset, u64 ino,
                    unsigned int type)
{
    struct dir_cursor *cursor = arg;
    char bounded[32];

    (void)offset;
    (void)ino;
    if (length <= 0 || length >= (int)sizeof(bounded) || name == NULL || name[0] == '.') {
        return 0;
    }
    memcpy(bounded, name, (size_t)length);
    bounded[length] = '\0';
    if ((cursor->after[0] != '\0' && strcmp(bounded, cursor->after) <= 0) ||
        (cursor->name[0] != '\0' && strcmp(bounded, cursor->name) >= 0)) {
        return 0;
    }
    strcpy(cursor->name, bounded);
    cursor->type = type;
    return 0;
}

/* 每次读取一个字典序后继项后立即关闭目录，避免目录锁与重入风险 */
static int dir_next(const char *path, struct dir_cursor *cursor)
{
    struct dentry *dentry = vfs_lookup_path(vfs_get_root(), path);
    struct file file;
    int ret, close_ret = 0;

    if (dentry == NULL || dentry->d_inode == NULL) {
        return 0;
    }
    if (!S_ISDIR(dentry->d_inode->i_mode)) {
        return -ENOTDIR;
    }
    if (dentry->d_inode->i_fop == NULL || dentry->d_inode->i_fop->readdir == NULL) {
        return -ENOTSUP;
    }
    memset(&file, 0, sizeof(file));
    file.f_inode = dentry->d_inode;
    file.f_op = file.f_inode->i_fop;
    file.f_path.dentry = dentry;
    cursor->name[0] = '\0';
    ret = file.f_op->open != NULL ? file.f_op->open(file.f_inode, &file) : 0;
    if (ret != 0) {
        return ret;
    }
    ret = file.f_op->readdir(&file, cursor, dir_fill);
    if (file.f_op->release != NULL) {
        close_ret = file.f_op->release(file.f_inode, &file);
    }
    if (ret < 0) {
        return ret;
    }
    if (close_ret != 0) {
        return close_ret;
    }
    if (cursor->name[0] == '\0') {
        return 0;
    }
    strcpy(cursor->after, cursor->name);
    return 1;
}

static int strip_nkc_name(const char *filename, char out[KB_STORE_NAME_SIZE])
{
    size_t len = strlen(filename);
    size_t stem_len;

    if (len <= 4U || strcmp(filename + len - 4U, ".nkc") != 0) {
        return 0;
    }
    stem_len = len - 4U;
    if (stem_len > KB_STORE_NAME_MAX) {
        return -EINVAL;
    }
    memcpy(out, filename, stem_len);
    out[stem_len] = '\0';
    return kb_store_valid_name(out) ? 1 : -EINVAL;
}

int kb_store_read_page(const struct kb_store *store, uint8_t kind, const char *name,
                       uint64_t revision, uint32_t offset, void *buffer, size_t length,
                       uint32_t *body_size)
{
    char path[96];
    const struct kb_store_entry *entry;
    uint32_t total;
    int fd, ret, close_ret;

    if (store == NULL || !store->ready || kind_dir(kind) == NULL || !kb_store_valid_name(name) ||
        (buffer == NULL && length != 0U) || revision == 0U) {
        return -EINVAL;
    }
    entry = kb_store_find(store, kind, name);
    if (entry == NULL ||
        (revision != KB_STORE_CURRENT_REVISION && revision != entry->revision)) {
        return -ENOENT;
    }
    revision = entry->revision;
    object_path(path, kind, name);
    ret = read_record(path, OBJECT_MAGIC, kind, revision, NULL, KB_STORE_MAX_BODY, &total, NULL,
                      &fd);
    if (ret != 0) {
        return ret;
    }
    if (offset > total) {
        ret = -EINVAL;
    } else if (buffer == NULL) {
        ret = offset == 0U ? (int)total : -EINVAL;
    } else {
        if (length > total - offset) {
            length = total - offset;
        }
        if (nos_lseek(fd, HEADER_SIZE + offset, SEEK_SET) < 0) {
            ret = -EIO;
        } else {
            ret = transfer(fd, buffer, length, 0);
            if (ret == 0) {
                ret = (int)length;
            }
        }
    }
    close_ret = nos_close(fd);
    if (ret >= 0 && close_ret == 0 && body_size != NULL) {
        *body_size = total;
    }
    return ret < 0 ? ret : (close_ret != 0 ? close_ret : ret);
}

int kb_store_read(const struct kb_store *store, uint8_t kind, const char *name, uint64_t revision,
                  uint32_t offset, void *buffer, size_t length)
{
    return kb_store_read_page(store, kind, name, revision, offset, buffer, length, NULL);
}

static int load_store(struct kb_store *store)
{
    uint8_t body[ROOT_SIZE];
    char path[96];
    uint32_t size, object_size;
    uint64_t root_seq = 0U, object_rev = 0U;
    uint8_t kind;
    const struct kb_store_entry *base_entry, *light_entry;
    int ret;

    ret = read_record(KB_STORE_ROOT_PATH, ROOT_MAGIC, 0, UINT64_MAX, body, ROOT_SIZE, &size,
                      &root_seq, NULL);
    if (ret != 0) {
        return ret;
    }
    if (size != ROOT_SIZE || body[2U * KB_STORE_NAME_SIZE] > 1U) {
        return -EINVAL;
    }
    if (decode_ref_name(&store->selection.base, body) != 0 ||
        decode_ref_name(&store->selection.light, body + KB_STORE_NAME_SIZE) != 0) {
        return -EINVAL;
    }
    store->selection.light_enabled = body[2U * KB_STORE_NAME_SIZE] != 0U;
    store->sequence = root_seq;

    for (kind = KB_STORE_BASE; kind <= KB_STORE_MACRO; kind++) {
        struct dir_cursor cursor = {0};

        while ((ret = dir_next(kind_dir(kind), &cursor)) > 0) {
            char name[KB_STORE_NAME_SIZE];
            struct kb_store_entry *e;
            int stem_ret;

            if (cursor.type == DT_DIR) {
                continue;
            }
            stem_ret = strip_nkc_name(cursor.name, name);
            if (stem_ret == 0) {
                continue;
            }
            if (stem_ret < 0) {
                return stem_ret;
            }
            if (store->count >= KB_STORE_MAX_OBJECTS) {
                return -ENOSPC;
            }
            if (kb_store_find(store, kind, name) != NULL) {
                return -EINVAL;
            }
            object_path(path, kind, name);
            ret = read_record(path, OBJECT_MAGIC, kind, UINT64_MAX, NULL, KB_STORE_MAX_BODY,
                              &object_size, &object_rev, NULL);
            if (ret != 0) {
                return ret;
            }
            e = &store->entries[store->count++];
            memset(e, 0, sizeof(*e));
            e->kind = kind;
            e->revision = object_rev;
            e->length = (uint16_t)object_size;
            strcpy(e->name, name);
            if (object_rev > store->sequence) {
                store->sequence = object_rev;
            }
        }
        if (ret < 0) {
            return ret;
        }
    }

    base_entry = kb_store_find(store, KB_STORE_BASE, store->selection.base.name);
    light_entry = kb_store_find(store, KB_STORE_LIGHT, store->selection.light.name);
    if (base_entry == NULL || light_entry == NULL) {
        return -EINVAL;
    }
    store->selection.base.revision = base_entry->revision;
    store->selection.light.revision = light_entry->revision;
    store->catalog_revision = store->sequence;
    return 0;
}

int kb_store_open(struct kb_store *store)
{
    int ret;

    if (store == NULL) {
        return -EINVAL;
    }
    /* 未完成全部对象校验前保持未就绪状态 */
    memset(store, 0, sizeof(*store));
    ret = load_store(store);
    if (ret != 0) {
        memset(store, 0, sizeof(*store));
        return ret;
    }
    store->ready = true;
    return 0;
}

static int write_object_atomic(const char *obj_path, uint8_t kind, uint64_t revision,
                               const void *body, uint16_t length, bool *rename_attempted)
{
    int ret;

    *rename_attempted = false;
    ret = write_record(KB_STORE_OBJECT_TEMP, OBJECT_MAGIC, kind, revision, body, length, 0);
    if (ret != 0) {
        return ret;
    }
    *rename_attempted = true;
    ret = nos_rename(KB_STORE_OBJECT_TEMP, obj_path);
    if (ret != 0) {
        if (object_matches(obj_path, kind, revision, body, length) == 1) {
            return 0;
        }
    }
    return ret;
}

static int stage_object(struct kb_store *next, uint8_t kind, const char *name, const void *body,
                        uint16_t length, struct kb_store_entry *entry, char obj_path[96],
                        bool *rename_attempted)
{
    int ret = prepare_dirs(kind);

    *rename_attempted = false;
    if (ret != 0) {
        return ret;
    }
    if (next->sequence >= UINT64_MAX - 1U) {
        return -EOVERFLOW;
    }
    next->sequence++;
    object_path(obj_path, kind, name);
    ret = write_object_atomic(obj_path, kind, next->sequence, body, length, rename_attempted);
    if (ret != 0) {
        return ret;
    }
    memset(entry, 0, sizeof(*entry));
    entry->kind = kind;
    entry->revision = next->sequence;
    entry->length = length;
    strcpy(entry->name, name);
    return 0;
}

static void select_entry(struct kb_store_ref *ref, const struct kb_store_entry *entry)
{
    memset(ref, 0, sizeof(*ref));
    ref->revision = entry->revision;
    strcpy(ref->name, entry->name);
}

int kb_store_seed(struct kb_store *store, const void *base, uint16_t base_length, const void *light,
                  uint16_t light_length)
{
    struct kb_store next = {0};
    char base_path[96] = {0}, light_path[96] = {0};
    bool base_written = false, light_written = false, rename_attempted = false;
    int fd, ret;

    if (store == NULL || base == NULL || light == NULL || base_length > KB_STORE_MAX_BODY ||
        light_length > KB_STORE_MAX_BODY) {
        return -EINVAL;
    }
    if (store->ready) {
        return -EEXIST;
    }
    /* 绝不覆盖已有（可能损坏）的根配置文件 */
    fd = nos_open(KB_STORE_ROOT_PATH, O_RDONLY);
    if (fd >= 0) {
        ret = nos_close(fd);
        return ret != 0 ? ret : -EEXIST;
    }
    if (fd != -ENOENT) {
        return fd;
    }
    ret = prepare_dirs(KB_STORE_MACRO);
    if (ret == 0) {
        ret = stage_object(&next, KB_STORE_BASE, "default", base, base_length, &next.entries[0],
                           base_path, &rename_attempted);
        base_written = (ret == 0);
    }
    if (ret == 0) {
        ret = stage_object(&next, KB_STORE_LIGHT, "default", light, light_length, &next.entries[1],
                           light_path, &rename_attempted);
        light_written = (ret == 0);
    }
    if (ret != 0) {
        if (base_written) {
            (void)nos_unlink(base_path);
        }
        return ret;
    }
    next.count = 2U;
    next.selection.light_enabled = true;
    select_entry(&next.selection.base, &next.entries[0]);
    select_entry(&next.selection.light, &next.entries[1]);
    ret = publish_root(store, &next, true);
    if (ret != 0 && root_matches(&next) != 1) {
        if (base_written) {
            (void)nos_unlink(base_path);
        }
        if (light_written) {
            (void)nos_unlink(light_path);
        }
    }
    return ret;
}

static int object_equal(const struct kb_store_entry *entry, const void *body, uint16_t length)
{
    char path[96];

    if (entry->length != length) {
        return 0;
    }
    object_path(path, entry->kind, entry->name);
    return object_matches(path, entry->kind, entry->revision, body, length);
}

int kb_store_begin_session(struct kb_store *store)
{
    struct kb_store next;
    int ret;

    if (store == NULL || !store->ready) {
        return -EINVAL;
    }
    next = *store;
    ret = publish_root(store, &next, false);
    if (ret != 0 && (!store->ready || root_matches(store) != 1)) {
        store->ready = false;
    }
    return ret;
}

int kb_store_save(struct kb_store *store, uint8_t kind, const char *name,
                  uint64_t expected_revision, const void *body, uint16_t length,
                  const struct kb_store_selection *selection)
{
    struct kb_store next;
    const struct kb_store_entry *old;
    struct kb_store_entry *entry;
    char obj_path[96];
    bool rename_attempted = false;
    int ret;

    if (store == NULL || !store->ready || kind_dir(kind) == NULL || !kb_store_valid_name(name) ||
        body == NULL || length > KB_STORE_MAX_BODY) {
        return -EINVAL;
    }
    if (selection != NULL && (!valid_ref(store, KB_STORE_BASE, &selection->base) ||
                              !valid_ref(store, KB_STORE_LIGHT, &selection->light))) {
        return -EINVAL;
    }
    old = kb_store_find(store, kind, name);
    if (expected_revision == 0U && old != NULL) {
        return -EEXIST;
    }
    if (expected_revision != 0U && (old == NULL || old->revision != expected_revision)) {
        return -ESTALE;
    }
    if (old == NULL && store->count == KB_STORE_MAX_OBJECTS) {
        return -ENOSPC;
    }
    next = *store;
    if (selection != NULL) {
        next.selection = *selection;
    }
    if (old != NULL) {
        ret = object_equal(old, body, length);
        if (ret < 0) {
            return ret;
        }
        if (ret != 0) {
            if (kind == KB_STORE_BASE) {
                select_entry(&next.selection.base, old);
            }
            if (kind == KB_STORE_LIGHT) {
                select_entry(&next.selection.light, old);
            }
            if (same_selection(&store->selection, &next.selection)) {
                return 0;
            }
            ret = publish_root(store, &next, false);
            if (ret != 0 && root_matches(store) != 1) {
                store->ready = false;
            }
            return ret;
        }
    }
    entry = old != NULL ? &next.entries[old - store->entries] : &next.entries[next.count++];
    ret = stage_object(&next, kind, name, body, length, entry, obj_path, &rename_attempted);
    if (ret != 0) {
        if (old != NULL && rename_attempted) {
            store->ready = false;
        }
        return ret;
    }
    if (kind == KB_STORE_BASE) {
        select_entry(&next.selection.base, entry);
    }
    if (kind == KB_STORE_LIGHT) {
        select_entry(&next.selection.light, entry);
    }
    ret = publish_root(store, &next, true);
    if (ret != 0) {
        if (old == NULL && root_matches(store) == 1) {
            (void)nos_unlink(obj_path);
        } else {
            /* 覆盖文件已替换或根发布结果不确定，标记未就绪以从磁盘同步 */
            store->ready = false;
        }
    }
    return ret;
}

int kb_store_delete(struct kb_store *store, uint8_t kind, const char *name,
                    uint64_t expected_revision)
{
    struct kb_store next;
    const struct kb_store_entry *entry;
    char obj_path[96];
    size_t index;
    int ret;

    if (store == NULL || !store->ready || kind_dir(kind) == NULL || !kb_store_valid_name(name)) {
        return -EINVAL;
    }
    entry = kb_store_find(store, kind, name);
    if (entry == NULL) {
        return -ENOENT;
    }
    if (entry->revision != expected_revision) {
        return -ESTALE;
    }
    if ((kind == KB_STORE_BASE && strcmp(store->selection.base.name, name) == 0) ||
        (kind == KB_STORE_LIGHT && strcmp(store->selection.light.name, name) == 0)) {
        return -EBUSY;
    }
    next = *store;
    index = (size_t)(entry - store->entries);
    memmove(&next.entries[index], &next.entries[index + 1U],
            (next.count - index - 1U) * sizeof(*entry));
    memset(&next.entries[--next.count], 0, sizeof(*entry));

    object_path(obj_path, kind, name);
    ret = nos_unlink(obj_path);
    if (ret != 0) {
        return ret;
    }
    ret = publish_root(store, &next, true);
    if (ret != 0) {
        store->ready = false;
    }
    return ret;
}

int kb_store_persist_selection(struct kb_store *store, const struct kb_store_selection *selection)
{
    struct kb_store next;
    int ret;

    if (store == NULL || !store->ready || selection == NULL ||
        !valid_ref(store, KB_STORE_BASE, &selection->base) ||
        !valid_ref(store, KB_STORE_LIGHT, &selection->light)) {
        return -EINVAL;
    }
    if (same_selection(&store->selection, selection)) {
        return 0;
    }
    next = *store;
    next.selection = *selection;
    ret = publish_root(store, &next, false);
    if (ret != 0 && root_matches(store) != 1) {
        store->ready = false;
    }
    return ret;
}

int kb_store_gc(struct kb_store *store)
{
    int ret, obj_ret;

    if (store == NULL || !store->ready) {
        return -EINVAL;
    }
    ret = nos_unlink(KB_STORE_ROOT_TEMP);
    if (ret == -ENOENT) {
        ret = 0;
    }
    obj_ret = nos_unlink(KB_STORE_OBJECT_TEMP);
    if (obj_ret == -ENOENT) {
        obj_ret = 0;
    }
    return ret != 0 ? ret : obj_ret;
}
