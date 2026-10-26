/* Copyright (C) 2026 NOS contributors. Real VFS with sector/heap fault injection. */
#define NOS_VFS_SNAPSHOT3 1
#ifndef HOST_BLOCK_SECTORS
#define HOST_BLOCK_SECTORS 224u
#endif
#define main legacy_vfs_test_main
#include "host_esp32_vfs.c"
#undef main

static unsigned stride(void) {
    return (unsigned)block_capacity / 2;
}
static void reboot_model(void) {
    fail_event = fail_read = fail_allocation = fail_growth = fail_write = 0;
    block_events = writes = reads = 0;
    snapshot3_active = snapshot3_cleanup = snapshot3_legacy = 0;
    reset_namespace();
}
static void legacy_image(void) {
    memset(disk, 0, sizeof(disk));
    for (unsigned bank = 0; bank < 2; bank++) {
        struct disk_node e = {.mode = MODE_DIR | 0755};
        strcpy(e.name, "/data");
        memcpy(disk[bank * BANK_SECTORS + 13], &e, sizeof(e));
        e = (struct disk_node){.mode = MODE_FILE | 0644, .size = 512};
        strcpy(e.name, "/data/file");
        memcpy(disk[bank * BANK_SECTORS + 15], &e, sizeof(e));
        memset(disk[bank * BANK_SECTORS + 16], 'A' + bank, 512);
        unsigned crc = ~0u;
        for (unsigned i = 1; i < BANK_SECTORS; i++)
            crc = crc32(crc, disk[bank * BANK_SECTORS + i], 512);
        struct disk_header h = {0x4e4f5346u, 2, 40 + bank, 2, ~crc, 0};
        h.header_crc = ~crc32(~0u, &h, offsetof(struct disk_header, header_crc));
        memcpy(disk[bank * BANK_SECTORS], &h, sizeof(h));
    }
}
static void append_pattern(int fd, unsigned char value, unsigned size) {
    for (unsigned i = 0; i < size; i += 64)
        assert(write_bytes(fd, value, 64) == 64);
}
static void check_file(unsigned size, unsigned char value) {
    int index = lookup("/data/file");
    assert(index >= 0 && nodes[index].size == size);
    unsigned char bytes[512];
    for (unsigned offset = 0; offset < size; offset += 512) {
        node_read_data(&nodes[index], offset, bytes, 512);
        for (unsigned j = 0; j < 512; j++)
            assert(bytes[j] == value);
    }
}
static void replace_file(unsigned char value, unsigned size) {
    int fd = path(NOS_VFS_OPEN, "/data/file", NOS_O_RDWR | NOS_O_TRUNC);
    assert(fd >= 3);
    append_pattern(fd, value, size);
    close_file(fd);
}
static void migration_faults(void) {
    unsigned char original[sizeof(disk)];
    legacy_image();
    memcpy(original, disk, sizeof(disk));
    for (unsigned fault = 0; fault <= 25; fault++) {
        reboot_model();
        memcpy(disk, original, sizeof(disk));
        assert(!disk_mount("/data") && !writes);
        assert(!memcmp(disk, original, sizeof(disk)));
        check_file(512, 'B');
        replace_file('C', 8192);
        block_events = 0;
        fail_event = fault;
        int rc = disk_sync();
        assert(!fault ? !rc : 1);
        unsigned char before_boot[sizeof(disk)];
        memcpy(before_boot, disk, sizeof(disk));
        reboot_model();
        assert(!disk_mount("/data") && !writes);
        assert(!memcmp(before_boot, disk, sizeof(disk)));
        int index = lookup("/data/file");
        assert(index >= 0);
        if (disk_generation == 41) {
            check_file(512, 'B');
            assert(!memcmp(original, disk, 66u * 512u));
        } else {
            assert(disk_generation == 42 && snapshot3_active == 1);
            check_file(8192, 'C');
        }
        assert(!disk_sync());
        struct disk_header h;
        assert(disk_bank_check(0, &h) && disk_bank_check(1, &h));
    }
    /* Retry retirement in the same boot: bank 0 is unavailable until it
     * finishes, including when the first retirement flush fails. */
    for (unsigned fault = 23; fault <= 25; fault++) {
        reboot_model();
        memcpy(disk, original, sizeof(disk));
        assert(!disk_mount("/data"));
        replace_file('C', 8192);
        block_events = 0;
        fail_event = fault;
        assert(disk_sync() == -NOS_EIO && snapshot3_cleanup && snapshot3_active == 1);
        fail_event = block_events + 1;
        assert(disk_sync() == -NOS_EIO && snapshot3_cleanup && snapshot3_active == 1);
        fail_event = 0;
        assert(!disk_sync() && !snapshot3_cleanup && snapshot3_active == 0);
        reboot_model();
        assert(!disk_mount("/data"));
        check_file(8192, 'C');
    }
    puts("PASS snapshot3_v2_read_only_import_and_every_migration_write_flush_cut");
}
static void recovery_faults(void) {
    reboot_model();
    memset(disk, 0, sizeof(disk));
    assert(!disk_mount("/data"));
    int fd = open_file("/data/file");
    append_pattern(fd, 'D', 8192);
    close_file(fd);
    assert(!disk_sync() && disk_generation == 1 && snapshot3_active == 1);
    unsigned char original[sizeof(disk)];
    memcpy(original, disk, sizeof(disk));
    reboot_model();
    assert(disk_mount_v2("/data") == -NOS_EIO && !writes);
    for (unsigned fault = 0; fault <= 22; fault++) {
        reboot_model();
        memcpy(disk, original, sizeof(disk));
        assert(!disk_mount("/data"));
        replace_file('E', 8192);
        block_events = 0;
        fail_event = fault;
        int rc = disk_sync();
        assert(!fault ? !rc : 1);
        reboot_model();
        assert(!disk_mount("/data") && !writes);
        check_file(8192, disk_generation == 1 ? 'D' : 'E');
    }
    reboot_model();
    memcpy(disk, original, sizeof(disk));
    assert(!disk_mount("/data"));
    replace_file('E', 8192);
    assert(!disk_sync());
    memcpy(original, disk, sizeof(disk));
    const unsigned corrupt[] = {0, 1, 3, 4, 19};
    for (unsigned i = 0; i < sizeof(corrupt) / sizeof(corrupt[0]); i++) {
        reboot_model();
        memcpy(disk, original, sizeof(disk));
        disk[corrupt[i]][7] ^= 0x40;
        assert(!disk_mount("/data") && disk_generation == 1 && !writes);
        check_file(8192, 'D');
    }
    reboot_model();
    disk[stride()][0] ^= 0x40;
    assert(disk_mount("/data") == -NOS_EIO && !writes && !disk_mounted);
    reboot_model();
    memset(disk, 0, sizeof(disk));
    disk[HOST_BLOCK_SECTORS - 1][511] = 1;
    assert(disk_mount("/data") == -NOS_EIO && !writes);
    puts("PASS snapshot3_commit_cuts_header_metadata_tail_crc_fallback_and_full_blank_scan");
}
static void recompute(unsigned bank) {
    struct snapshot3_header h;
    memcpy(&h, disk[bank * stride()], sizeof(h));
    unsigned crc = ~0u;
    for (unsigned i = 0; i < 3 + h.data_sectors; i++)
        crc = crc32(crc, disk[bank * stride() + 1 + i], 512);
    h.crc = ~crc;
    h.header_crc = ~crc32(~0u, &h, offsetof(struct snapshot3_header, header_crc));
    memcpy(disk[bank * stride()], &h, sizeof(h));
}
static void validation_and_rollback(void) {
    reboot_model();
    memset(disk, 0, sizeof(disk));
    assert(!disk_mount("/data"));
    int fd = open_file("/data/file");
    append_pattern(fd, 'F', 8192);
    close_file(fd);
    assert(!disk_sync());
    unsigned char original[sizeof(disk)];
    memcpy(original, disk, sizeof(disk));
    for (unsigned mutation = 0; mutation < 9; mutation++) {
        reboot_model();
        memcpy(disk, original, sizeof(disk));
        struct disk_node entries[DISK_NODE_COUNT];
        memcpy(entries, disk[stride() + 1], sizeof(entries));
        switch (mutation) {
        case 0:
            strcpy(entries[1].name, "/data");
            break;
        case 1:
            strcpy(entries[1].name, "/data/missing/file");
            break;
        case 2:
            entries[1].size = FILE_BYTES + 1;
            break;
        case 3:
            entries[1].mode = MODE_LINK;
            break;
        case 4:
            strcpy(entries[1].name, "/data/../escape");
            break;
        case 5:
            entries[0].mode = MODE_FILE;
            break;
        case 6:
            entries[2].mode = MODE_DIR;
            break;
        case 7:
            entries[1].size = 4096;
            break;
        case 8:
            memset(entries[1].name, 'z', sizeof(entries[1].name));
            break;
        }
        memcpy(disk[stride() + 1], entries, sizeof(entries));
        recompute(1);
        assert(disk_mount("/data") == -NOS_EIO && !writes && !disk_mounted);
        assert(lookup("/data") < 0 && !ram_backing_bytes);
    }
    for (unsigned mutation = 0; mutation < 5; mutation++) {
        reboot_model();
        memcpy(disk, original, sizeof(disk));
        struct snapshot3_header h;
        memcpy(&h, disk[stride()], sizeof(h));
        switch (mutation) {
        case 0:
            h.generation = 0;
            break;
        case 1:
            h.count = 0;
            break;
        case 2:
            h.count = DISK_NODE_COUNT + 1;
            break;
        case 3:
            h.bank_sectors--;
            break;
        case 4:
            h.data_sectors = h.bank_sectors;
            break;
        }
        h.header_crc = ~crc32(~0u, &h, offsetof(struct snapshot3_header, header_crc));
        memcpy(disk[stride()], &h, sizeof(h));
        assert(disk_mount("/data") == -NOS_EIO && !writes && !disk_mounted);
    }
    reboot_model();
    memcpy(disk, original, sizeof(disk));
    assert(!disk_mount("/data"));
    unsigned mount_reads = reads;
    for (unsigned fault = 1; fault <= mount_reads; fault++) {
        reboot_model();
        memcpy(disk, original, sizeof(disk));
        fail_read = fault;
        int rc = disk_mount("/data");
        assert(!writes && !memcmp(original, disk, sizeof(disk)));
        if (rc) {
            assert(rc == -NOS_EIO && !disk_mounted && lookup("/data") < 0 && !ram_backing_bytes);
        } else {
            check_file(8192, 'F');
        }
    }
    for (unsigned fault = 1; fault <= 16; fault++) {
        reboot_model();
        memcpy(disk, original, sizeof(disk));
        fail_allocation = allocations + fault;
        assert(disk_mount("/data") == -NOS_ENOMEM && !disk_mounted && !writes);
        assert(lookup("/data") < 0 && !ram_backing_bytes);
        fail_allocation = 0;
        assert(!disk_mount("/data"));
        check_file(8192, 'F');
    }
    reboot_model();
    memcpy(disk, original, sizeof(disk));
    for (unsigned i = 0; i < 4; i++) {
        char name[32];
        snprintf(name, sizeof(name), "/tmp/quota%u", i);
        int f = open_file(name);
        assert(!resize(f, 8192));
        close_file(f);
    }
    assert(disk_mount("/data") == -NOS_ENOSPC && !writes && !disk_mounted);
    remove_file("/tmp/quota0");
    assert(!disk_mount("/data"));
    check_file(8192, 'F');
    disk_generation = UINT32_MAX;
    assert(disk_sync() == -NOS_EOVERFLOW && !writes);
    reboot_model();
    memset(disk, 0, sizeof(disk));
    block_capacity = 135;
    assert(disk_mount("/data") == -NOS_ENOTSUP && !writes);
    block_capacity = UINT64_MAX;
    assert(disk_mount("/data") == -NOS_ENOTSUP && !writes);
    block_capacity = 136;
    assert(!disk_mount("/data"));
    for (unsigned i = 0; i < 4; i++) {
        char name[32];
        snprintf(name, sizeof(name), "/data/limit%u", i);
        int f = open_file(name);
        assert(!resize(f, 8192));
        close_file(f);
    }
    assert(!disk_sync());
    fd = open_file("/data/extra");
    assert(write_bytes(fd, 1, 64) == 64);
    close_file(fd);
    writes = 0;
    assert(disk_sync() == -NOS_ENOSPC && !writes);
    block_capacity = HOST_BLOCK_SECTORS;
    puts("PASS snapshot3_semantics_allocation_rollback_quota_geometry_and_overflow");
}
int main(void) {
    assert(!initialize_namespace());
    max_allocation = 512;
    migration_faults();
    recovery_faults();
    validation_and_rollback();
    reboot_model();
    for (unsigned i = 0; i < NODE_COUNT; i++)
        clear_node(i);
    nos_vfs_free(nodes);
    nos_vfs_free(opened);
    nodes = NULL;
    opened = NULL;
    fds = NULL;
    assert(!live_allocations);
    puts("PASS snapshot3_no_leaks");
    return 0;
}
