/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Author: Nick Hu
 *
 * Only config.nkc publishes a transaction. Orphan immutable revisions are never
 * discovered by directory scanning and are skipped when allocating revisions.
 */
#include <nos/keyboard_object_store.h>
#include <fs/fs.h>
#include <fs/vfs.h>
#include <kernel/errno.h>
#include <string.h>

#define MAX_REVISION_COLLISIONS 128U
#define HEADER_SIZE 24U
#define ENTRY_SIZE 36U
#define ROOT_SIZE 91U
#define CATALOG_SIZE (1U + KB_STORE_MAX_OBJECTS * ENTRY_SIZE)
#define OBJECT_MAGIC 0x324f424bU
#define CATALOG_MAGIC 0x3243424bU
#define ROOT_MAGIC 0x3252424bU

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

static int valid_id(const char *id, size_t limit)
{
    size_t i;
    if (id == NULL || id[0] == '\0') {
        return 0;
    }
    for (i = 0U; i < limit; i++) {
        unsigned char c = (unsigned char)id[i];
        if (c == '\0') {
            return i != 0U;
        }
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '-')) {
            return 0;
        }
    }
    return 0;
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

static void revision_path(char path[96], const char *dir, const char *id, uint64_t revision,
                          const char *ext)
{
    static const char hex[] = "0123456789abcdef";
    size_t n = strlen(dir);
    unsigned int i;
    memcpy(path, dir, n);
    path[n++] = '/';
    if (id != NULL) {
        size_t len = strlen(id);
        memcpy(path + n, id, len);
        n += len;
        path[n++] = '/';
    }
    for (i = 0U; i < 16U; i++) {
        path[n++] = hex[(revision >> (60U - 4U * i)) & 15U];
    }
    memcpy(path + n, ext, 5U);
}

static int mkdir_ok(const char *path)
{
    int ret = nos_mkdir(path, 0755);
    return ret == -EEXIST ? 0 : ret;
}

static int prepare_dirs(uint8_t kind, const char *id)
{
    char path[96];
    const char *dir = kind_dir(kind);
    int ret = mkdir_ok(KB_STORE_DIR);
    if (ret == 0) {
        ret = mkdir_ok(KB_STORE_CATALOG_DIR);
    }
    if (ret == 0 && dir != NULL) {
        ret = mkdir_ok(dir);
    }
    if (ret == 0 && id != NULL) {
        size_t n = strlen(dir);
        memcpy(path, dir, n);
        path[n++] = '/';
        strcpy(path + n, id);
        ret = mkdir_ok(path);
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
    put_le(header + 4, 1, 2);
    header[6] = kind;
    put_le(header + 8, revision, 8);
    put_le(header + 16, size, 4);
    crc = crc_update(crc_update(UINT32_MAX, header, 20), body, size) ^ UINT32_MAX;
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
/* Read and verify the entire record even for sliced reads, without a body-sized allocation. */
static int read_record(const char *path, uint32_t magic, uint8_t kind, uint64_t revision,
                       void *body, uint32_t capacity, uint32_t *size, int *out_fd)
{
    uint8_t header[HEADER_SIZE], scratch[128];
    uint32_t total, left, crc;
    int fd = nos_open(path, O_RDONLY), ret, close_ret;
    if (fd < 0) {
        return fd;
    }
    ret = transfer(fd, header, sizeof(header), 0);
    total = ret == 0 ? (uint32_t)get_le(header + 16, 4) : 0U;
    if (ret == 0 &&
        (get_le(header, 4) != magic || get_le(header + 4, 2) != 1U || header[6] != kind ||
         header[7] != 0U || get_le(header + 8, 8) == 0U || get_le(header + 8, 8) == UINT64_MAX ||
         (revision != UINT64_MAX && get_le(header + 8, 8) != revision) || total > capacity)) {
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
        if (out_fd != NULL) {
            *out_fd = fd;
            return 0;
        }
    }
    close_ret = nos_close(fd);
    return ret != 0 ? ret : close_ret;
}

const struct kb_store_entry *kb_store_find(const struct kb_store *store, uint8_t kind,
                                           const char *id)
{
    unsigned int i;
    if (store == NULL || kind_dir(kind) == NULL || !valid_id(id, KB_STORE_ID_SIZE)) {
        return NULL;
    }
    for (i = 0U; i < store->count; i++) {
        if (store->entries[i].kind == kind && strcmp(store->entries[i].id, id) == 0) {
            return &store->entries[i];
        }
    }
    return NULL;
}

static int valid_ref(const struct kb_store *store, uint8_t kind, const struct kb_store_ref *ref)
{
    const struct kb_store_entry *entry;
    if (!valid_id(ref->id, sizeof(ref->id))) {
        return 0;
    }
    entry = kb_store_find(store, kind, ref->id);
    return entry != NULL && entry->revision == ref->revision;
}

static void encode_ref(uint8_t *p, const struct kb_store_ref *ref)
{
    put_le(p, ref->revision, 8);
    memcpy(p + 8, ref->id, KB_STORE_REF_ID_SIZE);
}

static void decode_ref(struct kb_store_ref *ref, const uint8_t *p)
{
    ref->revision = get_le(p, 8);
    memcpy(ref->id, p + 8, KB_STORE_REF_ID_SIZE);
}

static int same_selection(const struct kb_store_selection *a, const struct kb_store_selection *b)
{
    return strcmp(a->base.id, b->base.id) == 0 && a->base.revision == b->base.revision &&
           strcmp(a->light.id, b->light.id) == 0 && a->light.revision == b->light.revision &&
           a->light_enabled == b->light_enabled;
}

static int root_matches(const struct kb_store *candidate)
{
    uint8_t body[ROOT_SIZE];
    uint32_t size;
    int ret =
        read_record(KB_STORE_ROOT_PATH, ROOT_MAGIC, 0, candidate->sequence, body, sizeof(body), &size, NULL);
    if (ret != 0) {
        return ret;
    }
    return size == ROOT_SIZE && get_le(body, 8) == candidate->catalog_revision &&
           get_le(body + 8, 8) == candidate->selection.base.revision &&
           memcmp(body + 16, candidate->selection.base.id, KB_STORE_REF_ID_SIZE) == 0 &&
           get_le(body + 49, 8) == candidate->selection.light.revision &&
           memcmp(body + 57, candidate->selection.light.id, KB_STORE_REF_ID_SIZE) == 0 &&
           body[90] == candidate->selection.light_enabled;
}

static int publish_catalog(struct kb_store *next)
{
    uint8_t body[CATALOG_SIZE] = {0};
    char path[96];
    unsigned int i, collisions = 0U;
    int ret;

    body[0] = next->count;
    for (i = 0U; i < next->count; i++) {
        uint8_t *p = body + 1U + ENTRY_SIZE * i;
        const struct kb_store_entry *e = &next->entries[i];

        put_le(p, e->revision, 8);
        put_le(p + 8, e->length, 2);
        p[10] = e->kind;
        memcpy(p + 11, e->id, KB_STORE_ID_SIZE);
    }
    do {
        if (collisions++ == MAX_REVISION_COLLISIONS) {
            return -ENOSPC;
        }
        if (next->sequence >= UINT64_MAX - 1U) {
            return -EOVERFLOW;
        }
        next->catalog_revision = ++next->sequence;
        revision_path(path, KB_STORE_CATALOG_DIR, NULL, next->catalog_revision, ".nki");
        ret = write_record(path, CATALOG_MAGIC, 0, next->catalog_revision, body,
                           1U + ENTRY_SIZE * next->count, 1);
    } while (ret == -EEXIST);
    return ret;
}

static int publish(struct kb_store *store, struct kb_store *next, int catalog)
{
    uint8_t body[ROOT_SIZE] = {0};
    int ret;

    if (catalog) {
        ret = publish_catalog(next);
        if (ret != 0) {
            return ret;
        }
    } else {
        if (next->sequence >= UINT64_MAX - 1U) {
            return -EOVERFLOW;
        }
        next->sequence++;
    }
    put_le(body, next->catalog_revision, 8);
    encode_ref(body + 8, &next->selection.base);
    encode_ref(body + 49, &next->selection.light);
    body[90] = next->selection.light_enabled;
    ret = write_record(KB_STORE_ROOT_TEMP, ROOT_MAGIC, 0, next->sequence, body, ROOT_SIZE, 0);
    if (ret == 0) {
        ret = nos_rename(KB_STORE_ROOT_TEMP, KB_STORE_ROOT_PATH);
        if (ret != 0) {
            /* A backend may durably replace the root before reporting an I/O
             * failure. Reconcile from its CRC-verified contents before GC can
             * discard either transaction's referenced immutable files. */
            if (root_matches(next) == 1) {
                ret = 0;
            } else if (!store->ready || root_matches(store) != 1) {
                store->ready = false;
            }
        }
    }
    if (ret == 0) {
        next->ready = true;
        *store = *next;
    }
    return ret;
}

int kb_store_read_page(const struct kb_store *store, uint8_t kind, const char *id, uint64_t revision,
                  uint32_t offset, void *buffer, size_t length, uint32_t *body_size)
{
    char path[96];
    const struct kb_store_entry *entry;
    uint32_t total;
    int fd, ret, close_ret;
    if (store == NULL || !store->ready || kind_dir(kind) == NULL ||
        !valid_id(id, KB_STORE_ID_SIZE) || (buffer == NULL && length != 0U) || revision == 0U) {
        return -EINVAL;
    }
    if (revision == KB_STORE_CURRENT_REVISION) {
        entry = kb_store_find(store, kind, id);
        if (entry == NULL) {
            return -ENOENT;
        }
        revision = entry->revision;
    }
    revision_path(path, kind_dir(kind), id, revision, ".nkc");
    ret = read_record(path, OBJECT_MAGIC, kind, revision, NULL, KB_STORE_MAX_BODY, &total, &fd);
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

int kb_store_read(const struct kb_store *store, uint8_t kind, const char *id, uint64_t revision,
                  uint32_t offset, void *buffer, size_t length)
{
    return kb_store_read_page(store, kind, id, revision, offset, buffer, length, NULL);
}

static int load_store(struct kb_store *store)
{
    uint8_t body[CATALOG_SIZE];
    char path[96];
    uint32_t size, object_size;
    unsigned int i, j;
    int fd, ret;
    ret = read_record(KB_STORE_ROOT_PATH, ROOT_MAGIC, 0, UINT64_MAX, body, ROOT_SIZE, &size, &fd);
    if (ret != 0) {
        return ret;
    }
    if (nos_lseek(fd, 8, SEEK_SET) < 0 || transfer(fd, path, 8, 0) != 0) {
        nos_close(fd);
        return -EIO;
    }
    store->sequence = get_le((uint8_t *)path, 8);
    ret = nos_close(fd);
    if (ret != 0) {
        return ret;
    }
    if (size != ROOT_SIZE || body[90] > 1U) {
        return -EINVAL;
    }
    store->catalog_revision = get_le(body, 8);
    if (store->catalog_revision == 0U || store->catalog_revision > store->sequence) {
        return -EINVAL;
    }
    decode_ref(&store->selection.base, body + 8);
    decode_ref(&store->selection.light, body + 49);
    store->selection.light_enabled = body[90] != 0U;
    revision_path(path, KB_STORE_CATALOG_DIR, NULL, store->catalog_revision, ".nki");
    ret =
        read_record(path, CATALOG_MAGIC, 0, store->catalog_revision, body, sizeof(body), &size, NULL);
    if (ret != 0) {
        return ret;
    }
    if (size < 1U || body[0] > KB_STORE_MAX_OBJECTS || size != 1U + ENTRY_SIZE * body[0]) {
        return -EINVAL;
    }
    store->count = body[0];
    for (i = 0U; i < store->count; i++) {
        const uint8_t *p = body + 1U + ENTRY_SIZE * i;
        struct kb_store_entry *e = &store->entries[i];
        e->revision = get_le(p, 8);
        e->length = (uint16_t)get_le(p + 8, 2);
        e->kind = p[10];
        memcpy(e->id, p + 11, KB_STORE_ID_SIZE);
        if (kind_dir(e->kind) == NULL || !valid_id(e->id, sizeof(e->id)) || e->revision == 0U ||
            e->revision >= store->catalog_revision || e->length > KB_STORE_MAX_BODY || p[35] != 0U) {
            return -EINVAL;
        }
        for (j = 0U; j < i; j++) {
            if (store->entries[j].kind == e->kind && strcmp(store->entries[j].id, e->id) == 0) {
                return -EINVAL;
            }
        }
        revision_path(path, kind_dir(e->kind), e->id, e->revision, ".nkc");
        ret = read_record(path, OBJECT_MAGIC, e->kind, e->revision, NULL, KB_STORE_MAX_BODY,
                          &object_size, NULL);
        if (ret != 0) {
            return ret;
        }
        if (object_size != e->length) {
            return -EINVAL;
        }
    }
    if (!valid_ref(store, KB_STORE_BASE, &store->selection.base) ||
        !valid_ref(store, KB_STORE_LIGHT, &store->selection.light)) {
        return -EINVAL;
    }
    return 0;
}

int kb_store_open(struct kb_store *store)
{
    int ret;

    if (store == NULL) {
        return -EINVAL;
    }
    /* The store is private to the worker. Decode into its unpublished state
     * and expose the catalog only after every referenced object is verified. */
    memset(store, 0, sizeof(*store));
    ret = load_store(store);
    if (ret != 0) {
        memset(store, 0, sizeof(*store));
        return ret;
    }
    store->ready = true;
    return 0;
}

static int stage_object(struct kb_store *next, uint8_t kind, const char *id, const void *body,
                        uint16_t length, struct kb_store_entry *entry)
{
    char path[96];
    unsigned int collisions = 0U;
    int ret = prepare_dirs(kind, id);
    if (ret != 0) {
        return ret;
    }
    do {
        if (collisions++ == MAX_REVISION_COLLISIONS) {
            return -ENOSPC;
        }
        if (next->sequence >= UINT64_MAX - 1U) {
            return -EOVERFLOW;
        }
        next->sequence++;
        revision_path(path, kind_dir(kind), id, next->sequence, ".nkc");
        ret = write_record(path, OBJECT_MAGIC, kind, next->sequence, body, length, 1);
    } while (ret == -EEXIST);
    if (ret != 0) {
        return ret;
    }
    memset(entry, 0, sizeof(*entry));
    entry->kind = kind;
    entry->revision = next->sequence;
    entry->length = length;
    strcpy(entry->id, id);
    return 0;
}

static void select_entry(struct kb_store_ref *ref, const struct kb_store_entry *entry)
{
    memset(ref, 0, sizeof(*ref));
    ref->revision = entry->revision;
    strcpy(ref->id, entry->id);
}

int kb_store_seed(struct kb_store *store, const void *base, uint16_t base_length, const void *light,
                  uint16_t light_length)
{
    struct kb_store next = {0};
    int fd, ret;
    if (store == NULL || base == NULL || light == NULL || base_length > KB_STORE_MAX_BODY ||
        light_length > KB_STORE_MAX_BODY) {
        return -EINVAL;
    }
    if (store->ready) {
        return -EEXIST;
    }
    /* Never replace an existing, possibly corrupt root with defaults. */
    fd = nos_open(KB_STORE_ROOT_PATH, O_RDONLY);
    if (fd >= 0) {
        ret = nos_close(fd);
        return ret != 0 ? ret : -EEXIST;
    }
    if (fd != -ENOENT) {
        return fd;
    }
    ret = stage_object(&next, KB_STORE_BASE, "default", base, base_length, &next.entries[0]);
    if (ret == 0) {
        ret = stage_object(&next, KB_STORE_LIGHT, "default", light, light_length, &next.entries[1]);
    }
    if (ret != 0) {
        return ret;
    }
    next.count = 2U;
    next.selection.light_enabled = true;
    select_entry(&next.selection.base, &next.entries[0]);
    select_entry(&next.selection.light, &next.entries[1]);
    return publish(store, &next, 1);
}

static int object_equal(const struct kb_store_entry *entry, const void *body, uint16_t length)
{
    char path[96];
    uint8_t scratch[128];
    uint32_t total, offset = 0U;
    int fd, ret, equal = 1;
    if (entry->length != length) {
        return 0;
    }
    revision_path(path, kind_dir(entry->kind), entry->id, entry->revision, ".nkc");
    ret = read_record(path, OBJECT_MAGIC, entry->kind, entry->revision, NULL, KB_STORE_MAX_BODY,
                      &total, &fd);
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
        offset += n;
    }
    {
        int close_ret = nos_close(fd);
        if (ret == 0) {
            ret = close_ret;
        }
    }
    return ret != 0 ? ret : equal;
}

int kb_store_begin_session(struct kb_store *store)
{
    struct kb_store next;
    if (store == NULL || !store->ready) {
        return -EINVAL;
    }
    next = *store;
    return publish(store, &next, 0);
}

int kb_store_save(struct kb_store *store, uint8_t kind, const char *id, uint64_t expected_revision,
                  const void *body, uint16_t length, const struct kb_store_selection *selection)
{
    struct kb_store next;
    const struct kb_store_entry *old;
    struct kb_store_entry *entry;
    int ret;
    if (store == NULL || !store->ready || kind_dir(kind) == NULL ||
        !valid_id(id, KB_STORE_ID_SIZE) || body == NULL || length > KB_STORE_MAX_BODY) {
        return -EINVAL;
    }
    if (selection != NULL && (!valid_ref(store, KB_STORE_BASE, &selection->base) ||
                              !valid_ref(store, KB_STORE_LIGHT, &selection->light))) {
        return -EINVAL;
    }
    old = kb_store_find(store, kind, id);
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
            return same_selection(&store->selection, &next.selection) ? 0
                                                                      : publish(store, &next, 0);
        }
    }
    entry = old != NULL ? &next.entries[old - store->entries] : &next.entries[next.count++];
    ret = stage_object(&next, kind, id, body, length, entry);
    if (ret != 0) {
        return ret;
    }
    if (kind == KB_STORE_BASE) {
        select_entry(&next.selection.base, entry);
    }
    if (kind == KB_STORE_LIGHT) {
        select_entry(&next.selection.light, entry);
    }
    return publish(store, &next, 1);
}

int kb_store_delete(struct kb_store *store, uint8_t kind, const char *id,
                    uint64_t expected_revision)
{
    struct kb_store next;
    const struct kb_store_entry *entry;
    size_t index;
    if (store == NULL || !store->ready || kind_dir(kind) == NULL ||
        !valid_id(id, KB_STORE_ID_SIZE)) {
        return -EINVAL;
    }
    entry = kb_store_find(store, kind, id);
    if (entry == NULL) {
        return -ENOENT;
    }
    if (entry->revision != expected_revision) {
        return -ESTALE;
    }
    if ((kind == KB_STORE_BASE && strcmp(store->selection.base.id, id) == 0) ||
        (kind == KB_STORE_LIGHT && strcmp(store->selection.light.id, id) == 0)) {
        return -EBUSY;
    }
    next = *store;
    index = (size_t)(entry - store->entries);
    memmove(&next.entries[index], &next.entries[index + 1U],
            (next.count - index - 1U) * sizeof(*entry));
    memset(&next.entries[--next.count], 0, sizeof(*entry));
    return publish(store, &next, 1);
}

int kb_store_persist_selection(struct kb_store *store, const struct kb_store_selection *selection)
{
    struct kb_store next;
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
    return publish(store, &next, 0);
}

struct gc_cursor {
    char after[32];
    char name[32];
    unsigned int type;
};
static int gc_fill(void *arg, const char *name, int length, loff_t offset, u64 ino,
                   unsigned int type)
{
    struct gc_cursor *cursor = arg;
    char bounded[32];
    (void)offset;
    (void)ino;
    if (length <= 0 || length >= (int)sizeof(bounded) || name == NULL || name[0] == '.') {
        return 0;
    }
    memcpy(bounded, name, (size_t)length);
    bounded[length] = '\0';
    if (strcmp(bounded, cursor->after) <= 0 ||
        (cursor->name[0] != '\0' && strcmp(bounded, cursor->name) >= 0)) {
        return 0;
    }
    strcpy(cursor->name, bounded);
    cursor->type = type;
    return 0;
}
/* Rescan after releasing the directory handle: unlink can reorder readdir, and
 * filesystem callbacks must not be reentered while its directory lock is held. */
static int gc_next(const char *path, struct gc_cursor *cursor)
{
    struct dentry *dentry;
    dentry = vfs_lookup_path(vfs_get_root(), path);
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
    ret = file.f_op->readdir(&file, cursor, gc_fill);
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

static int gc_revision(const char *name, const char *ext, uint64_t *revision)
{
    unsigned int i;
    uint64_t value = 0;
    if (strlen(name) != 20U || strcmp(name + 16, ext) != 0) {
        return 0;
    }
    for (i = 0U; i < 16U; i++) {
        unsigned char c = (unsigned char)name[i];
        unsigned int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10U;
        } else {
            return 0;
        }
        value = (value << 4) | digit;
    }
    *revision = value;
    return 1;
}

static int gc_versions(struct kb_store *store, const char *dir, uint8_t kind, const char *id)
{
    struct gc_cursor cursor = {0};
    const struct kb_store_entry *entry = id != NULL ? kb_store_find(store, kind, id) : NULL;
    const struct kb_store_ref *active =
        kind == KB_STORE_BASE ? &store->selection.base
                              : (kind == KB_STORE_LIGHT ? &store->selection.light : NULL);
    char path[96];
    uint64_t revision;
    int ret;
    while ((ret = gc_next(dir, &cursor)) > 0) {
        size_t n;
        if (cursor.type == DT_DIR ||
            !gc_revision(cursor.name, id != NULL ? ".nkc" : ".nki", &revision)) {
            continue;
        }
        if ((id == NULL && revision == store->catalog_revision) ||
            (entry != NULL && revision == entry->revision) ||
            (id != NULL && active != NULL && strcmp(active->id, id) == 0 &&
             revision == active->revision)) {
            continue;
        }
        n = strlen(dir);
        memcpy(path, dir, n);
        path[n++] = '/';
        strcpy(path + n, cursor.name);
        ret = nos_unlink(path);
        if (ret != 0 && ret != -ENOENT) {
            return ret;
        }
    }
    return ret;
}

int kb_store_gc(struct kb_store *store)
{
    struct gc_cursor cursor;
    uint8_t kind;
    char path[96];
    int ret;
    if (store == NULL || !store->ready) {
        return -EINVAL;
    }
    ret = gc_versions(store, KB_STORE_CATALOG_DIR, 0, NULL);
    if (ret != 0) {
        return ret;
    }
    for (kind = KB_STORE_BASE; kind <= KB_STORE_MACRO; kind++) {
        const char *dir = kind_dir(kind);
        memset(&cursor, 0, sizeof(cursor));
        while ((ret = gc_next(dir, &cursor)) > 0) {
            size_t n;
            if (cursor.type != DT_DIR || !valid_id(cursor.name, KB_STORE_ID_SIZE)) {
                continue;
            }
            n = strlen(dir);
            memcpy(path, dir, n);
            path[n++] = '/';
            strcpy(path + n, cursor.name);
            ret = gc_versions(store, path, kind, cursor.name);
            if (ret != 0) {
                return ret;
            }
            ret = nos_rmdir(path);
            if (ret != 0 && ret != -ENOTEMPTY && ret != -ENOENT) {
                return ret;
            }
        }
        if (ret < 0) {
            return ret;
        }
    }
    ret = nos_unlink(KB_STORE_ROOT_TEMP);
    return ret == -ENOENT ? 0 : ret;
}
