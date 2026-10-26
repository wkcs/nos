/* Copyright (C) 2026 NOS contributors. Optional sector-streamed snapshot format. */
#ifndef NOS_ESP_VFS
#error "Snapshot v3 requires the ESP paged file backing"
#endif
#define SNAPSHOT3_META_SECTORS 3u
#define SNAPSHOT3_DATA_START (1u + SNAPSHOT3_META_SECTORS)
struct snapshot3_header {
    uint32_t magic, version, generation, count, bank_sectors, data_sectors, crc, header_crc;
};
_Static_assert(sizeof(struct disk_node) * DISK_NODE_COUNT <= SNAPSHOT3_META_SECTORS * 512u,
               "snapshot metadata fits three sectors");
static unsigned snapshot3_active, snapshot3_cleanup, snapshot3_legacy;

static int snapshot3_geometry(unsigned *stride) {
    uint64_t sectors = nos_block_capacity();
    /* Bank 1 must lie beyond both legacy banks throughout migration. */
    if (sectors < 136 || sectors > UINT32_MAX)
        return -NOS_ENOTSUP;
    *stride = (unsigned)(sectors / 2);
    return 0;
}
static int snapshot3_entries(const struct snapshot3_header *h, struct disk_node *entries,
                             const char *path) {
    unsigned roots = 0, data = 0;
    for (unsigned i = 0; i < DISK_NODE_COUNT; i++) {
        struct disk_node *e = &entries[i];
        if (i >= h->count) {
            const unsigned char *bytes = (const unsigned char *)e;
            for (unsigned j = 0; j < sizeof(*e); j++)
                if (bytes[j])
                    return -NOS_EINVAL;
            continue;
        }
        if (e->name[63] || !e->name[0] || e->size > FILE_BYTES ||
            (NODE_TYPE(e) != MODE_FILE && NODE_TYPE(e) != MODE_DIR) ||
            (NODE_TYPE(e) == MODE_DIR && e->size))
            return -NOS_EINVAL;
        char canonical[VFS_PERSIST_PATH_MAX];
        int rc = normalize(e->name, u_strlen(e->name) + 1, canonical);
        if (rc || !u_streq(canonical, e->name) ||
            !(u_streq(e->name, path) || child_path(e->name, path)))
            return -NOS_EACCES;
        if (u_streq(e->name, path)) {
            if (NODE_TYPE(e) != MODE_DIR)
                return -NOS_ENOTDIR;
            roots++;
        } else {
            /* Validate parent completeness before installing any vnode. */
            char parent[64];
            u_memcpy(parent, e->name, u_strlen(e->name) + 1);
            size_t n = u_strlen(parent);
            while (n && parent[n - 1] != '/')
                n--;
            if (n > 1)
                n--;
            parent[n] = 0;
            unsigned found = 0;
            for (unsigned j = 0; j < h->count; j++)
                if (!entries[j].name[63] && NODE_TYPE(&entries[j]) == MODE_DIR &&
                    u_streq(entries[j].name, parent))
                    found++;
            if (found != 1)
                return -NOS_EINVAL;
        }
        for (unsigned j = 0; j < i; j++)
            if (u_streq(entries[j].name, e->name))
                return -NOS_EINVAL;
        data += (e->size + 511u) / 512u;
    }
    return roots == 1 && data == h->data_sectors ? 0 : -NOS_EINVAL;
}
static int snapshot3_check(unsigned bank, unsigned stride, const char *path,
                           struct snapshot3_header *h, struct disk_node *entries) {
    unsigned char sector[512];
    unsigned base = bank * stride;
    int rc = nos_block_read(base, sector);
    if (rc)
        return rc;
    u_memcpy(h, sector, sizeof(*h));
    if (h->magic != 0x4e4f5346u || h->version != 3 || !h->generation || !h->count ||
        h->count > DISK_NODE_COUNT || h->bank_sectors != stride ||
        h->data_sectors > stride - SNAPSHOT3_DATA_START ||
        h->header_crc != ~crc32(~0u, h, offsetof(struct snapshot3_header, header_crc)))
        return -NOS_EINVAL;
    unsigned crc = ~0u;
    for (unsigned i = 0; i < SNAPSHOT3_META_SECTORS + h->data_sectors; i++) {
        rc = nos_block_read(base + 1 + i, sector);
        if (rc)
            return rc;
        crc = crc32(crc, sector, sizeof(sector));
        if (i < SNAPSHOT3_META_SECTORS) {
            size_t offset = i * 512u;
            size_t amount = sizeof(struct disk_node) * DISK_NODE_COUNT - offset;
            if (amount > 512)
                amount = 512;
            u_memcpy((char *)entries + offset, sector, amount);
            for (size_t j = amount; j < sizeof(sector); j++)
                if (sector[j])
                    return -NOS_EINVAL;
        }
    }
    if (~crc != h->crc)
        return -NOS_EINVAL;
    return snapshot3_entries(h, entries, path) ? -NOS_EINVAL : 0;
}
static int snapshot3_retire_legacy(void) {
    if (!snapshot3_cleanup)
        return 0;
    unsigned char tombstone[512] = {0};
    /* Legacy firmware scans only sectors 0..65 when both headers are
     * invalid. A nonzero marker also rejects downgrade of a fresh v3 disk. */
    const uint32_t retired[2] = {0x4e4f5358u, 3};
    u_memcpy(tombstone, retired, sizeof(retired));
    int rc = nos_block_write(0, tombstone);
    if (!rc)
        rc = nos_block_write(BANK_SECTORS, tombstone);
    if (!rc)
        rc = nos_block_flush();
    if (!rc)
        snapshot3_cleanup = 0;
    return rc;
}
static int disk_sync(void) {
    if (!disk_mounted)
        return -NOS_ENOTSUP;
    if (disk_generation == UINT32_MAX)
        return -NOS_EOVERFLOW;
    unsigned stride;
    int rc = snapshot3_geometry(&stride);
    if (rc)
        return rc;
    rc = snapshot3_retire_legacy();
    if (rc)
        return rc;
    struct disk_node entries[DISK_NODE_COUNT] = {0};
    unsigned indices[DISK_NODE_COUNT], count = 0, data = 0;
    for (unsigned i = 0; i < NODE_COUNT; i++) {
        struct vnode *n = &nodes[i];
        if (!n->used || !n->linked || n->persistent != 1)
            continue;
        if (count == DISK_NODE_COUNT)
            return -NOS_ENOSPC;
        if (u_strlen(n->name) >= 64 || n->size > FILE_BYTES)
            return -NOS_ENAMETOOLONG;
        entries[count].mode = n->mode;
        entries[count].size = (uint32_t)n->size;
        u_memcpy(entries[count].name, n->name, u_strlen(n->name) + 1);
        indices[count++] = i;
        data += (unsigned)((n->size + 511u) / 512u);
    }
    if (!count || data > stride - SNAPSHOT3_DATA_START)
        return -NOS_ENOSPC;
    unsigned bank = snapshot3_active ^ 1u, base = bank * stride, crc = ~0u;
    unsigned char sector[512];
    for (unsigned i = 0; i < SNAPSHOT3_META_SECTORS; i++) {
        u_memset(sector, 0, sizeof(sector));
        size_t offset = i * 512u, amount = sizeof(entries) - offset;
        if (amount > 512)
            amount = 512;
        u_memcpy(sector, (char *)entries + offset, amount);
        crc = crc32(crc, sector, sizeof(sector));
        rc = nos_block_write(base + 1 + i, sector);
        if (rc)
            return rc;
    }
    unsigned position = base + SNAPSHOT3_DATA_START;
    for (unsigned i = 0; i < count; i++) {
        struct vnode *n = &nodes[indices[i]];
        for (size_t offset = 0; offset < n->size; offset += 512) {
            size_t amount = n->size - offset;
            if (amount > 512)
                amount = 512;
            u_memset(sector, 0, sizeof(sector));
            node_read_data(n, offset, sector, amount);
            crc = crc32(crc, sector, sizeof(sector));
            rc = nos_block_write(position++, sector);
            if (rc)
                return rc;
        }
    }
    rc = nos_block_flush();
    if (rc)
        return rc;
    struct snapshot3_header h = {0x4e4f5346u, 3, disk_generation + 1, count, stride, data, ~crc, 0};
    h.header_crc = ~crc32(~0u, &h, offsetof(struct snapshot3_header, header_crc));
    u_memset(sector, 0, sizeof(sector));
    u_memcpy(sector, &h, sizeof(h));
    rc = nos_block_write(base, sector);
    if (!rc)
        rc = nos_block_flush();
    if (rc)
        return rc;
    /* Publication precedes legacy retirement. A failed retirement must be
     * retried before bank 0 (and its overlapping legacy payload) is reused. */
    snapshot3_active = bank;
    disk_generation = h.generation;
    if (snapshot3_legacy) {
        snapshot3_cleanup = 1;
        snapshot3_legacy = 0;
    }
    return snapshot3_retire_legacy();
}
static int disk_mount(const char *path) {
    if (u_strlen(path) >= 64)
        return -NOS_ENAMETOOLONG;
    if (disk_mounted || fat_mounted)
        return -NOS_EBUSY;
    if (backend(path) || u_streq(path, "/"))
        return -NOS_EACCES;
    int root = lookup(path);
    if (root >= 0 && NODE_TYPE(&nodes[root]) != MODE_DIR)
        return -NOS_ENOTDIR;
    for (unsigned i = 0; i < NODE_COUNT; i++)
        if (nodes[i].used && nodes[i].linked && child_path(nodes[i].name, path))
            return -NOS_ENOTEMPTY;
    int rc = nos_block_init();
    if (rc)
        return rc;
    unsigned stride;
    rc = snapshot3_geometry(&stride);
    if (rc)
        return rc;
    struct snapshot3_header headers[2];
    struct disk_node entries[DISK_NODE_COUNT];
    int valid[2];
    for (unsigned i = 0; i < 2; i++) {
        rc = snapshot3_check(i, stride, path, &headers[i], entries);
        if (rc && rc != -NOS_EINVAL)
            return rc;
        valid[i] = !rc;
    }
    struct disk_header legacy;
    unsigned legacy_valid = !disk_bank_check(0, &legacy) || !disk_bank_check(1, &legacy);
    if (!valid[0] && !valid[1]) {
        if (!legacy_valid) {
            unsigned char sector[512];
            /* Scan the entire geometry, including data beyond legacy banks. */
            for (unsigned i = 0; i < (unsigned)nos_block_capacity(); i++) {
                rc = nos_block_read(i, sector);
                if (rc)
                    return rc;
                for (unsigned j = 0; j < sizeof(sector); j++)
                    if (sector[j])
                        return -NOS_EIO;
            }
        }
        rc = disk_mount_v2(path);
        if (!rc) {
            snapshot3_active = 0; /* First commit must preserve sectors 0..65. */
            snapshot3_cleanup = 0;
            snapshot3_legacy = 1;
            /* Retirement is deferred until the first v3 header is durable. */
        }
        return rc;
    }
    unsigned bank = valid[1] && (!valid[0] || headers[1].generation > headers[0].generation);
    rc = snapshot3_check(bank, stride, path, &headers[bank], entries);
    if (rc)
        return rc;
    unsigned free_nodes = root >= 0, needed = 0;
    for (unsigned i = 0; i < NODE_COUNT; i++)
        free_nodes += !nodes[i].used;
    for (unsigned i = 0; i < headers[bank].count; i++)
        if (entries[i].size > INLINE_BYTES)
            needed += (entries[i].size + 511u) & ~511u;
    if (free_nodes < headers[bank].count || needed > RAM_BACKING_BYTES - ram_backing_bytes)
        return -NOS_ENOSPC;
    unsigned created = root < 0, installed = 0;
    unsigned root_mode = created ? MODE_DIR : nodes[root].mode;
    if (created)
        root = create(path, MODE_DIR);
    if (root < 0)
        return root;
    nodes[root].persistent = 1;
    unsigned position = bank * stride + SNAPSHOT3_DATA_START;
    unsigned char sector[512];
    for (unsigned i = 0; i < headers[bank].count; i++) {
        struct disk_node *e = &entries[i];
        if (u_streq(e->name, path)) {
            nodes[root].mode = e->mode;
            continue;
        }
        unsigned slot = 0;
        while (slot < NODE_COUNT && nodes[slot].used)
            slot++;
        if (slot == NODE_COUNT) {
            rc = -NOS_ENOSPC;
            goto rollback;
        }
        struct vnode *n = &nodes[slot];
        clear_node(slot);
        u_memcpy(n->name, e->name, u_strlen(e->name) + 1);
        n->mode = e->mode;
        n->used = n->linked = n->persistent = 1;
        installed |= 1u << slot;
        rc = node_reserve(n, e->size);
        if (rc)
            goto rollback;
        for (size_t offset = 0; offset < e->size; offset += 512) {
            rc = nos_block_read(position++, sector);
            if (rc)
                goto rollback;
            size_t amount = e->size - offset;
            if (amount > 512)
                amount = 512;
            node_write_data(n, offset, sector, amount);
        }
        n->size = e->size;
    }
    disk_generation = headers[bank].generation;
    snapshot3_active = bank;
    snapshot3_legacy = 0;
    snapshot3_cleanup = bank == 1 && !valid[0];
    u_memcpy(mount_path, path, u_strlen(path) + 1);
    disk_mounted = 1;
    return 0;
rollback:
    for (unsigned i = 0; i < NODE_COUNT; i++)
        if (installed & (1u << i))
            clear_node(i);
    nodes[root].persistent = 0;
    nodes[root].mode = root_mode;
    if (created)
        clear_node((unsigned)root);
    return rc;
}
