/* Copyright (C) 2026 NOS contributors. Actual ESP VFS, mocked IPC/heap/block I/O. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../servers/vfs/server.c"

static unsigned allocations, fail_allocation, live_allocations, fail_growth;
static size_t max_allocation = SIZE_MAX;
static unsigned writes, fail_write, fat_flushes;
static int fat_error;
static unsigned char disk[66][512];
void *nos_vfs_allocate(size_t bytes) {
    if (++allocations == fail_allocation || fail_growth || bytes > max_allocation)
        return NULL;
    void *p = calloc(1, bytes);
    live_allocations += p != NULL;
    return p;
}
void nos_vfs_free(void *address) {
    if (address) {
        assert(live_allocations);
        live_allocations--;
    }
    free(address);
}
void *u_memcpy(void *out, const void *in, size_t count) {
    return memcpy(out, in, count);
}
void *u_memset(void *out, int value, size_t count) {
    return memset(out, value, count);
}
size_t u_strlen(const char *text) {
    return strlen(text);
}
int u_streq(const char *a, const char *b) {
    return !strcmp(a, b);
}
intptr_t nos_syscall(uintptr_t op, uintptr_t a, uintptr_t b, uintptr_t c) {
    (void)op;
    (void)a;
    (void)b;
    (void)c;
    return -NOS_EACCES;
}
void nos_uart_init(void) {}
int nos_uart_readable(void) {
    return 0;
}
int nos_uart_writable(void) {
    return 1;
}
void nos_uart_get_stats(struct nos_uart_stats *stats) {
    memset(stats, 0, sizeof(*stats));
}
intptr_t nos_device_uart_read(void) {
    return -NOS_EAGAIN;
}
intptr_t nos_device_uart_write(const void *data, size_t count) {
    (void)data;
    return count;
}
intptr_t nos_random_read(void *out, size_t count, unsigned flags) {
    (void)out;
    (void)count;
    (void)flags;
    return -NOS_ENOTSUP;
}
int nos_block_init(void) {
    return 0;
}
uint64_t nos_block_capacity(void) {
    return 66;
}
uint64_t nos_block_interrupts(void) {
    return 0;
}
uint64_t nos_block_requests(void) {
    return writes;
}
uint64_t nos_block_poll_fallbacks(void) {
    return 0;
}
unsigned nos_block_version(void) {
    return 1;
}
int nos_block_read(unsigned sector, void *out) {
    assert(sector < 66);
    memcpy(out, disk[sector], 512);
    return 0;
}
int nos_block_write(unsigned sector, const void *in) {
    assert(sector < 66);
    writes++;
    if (fail_write)
        return -NOS_EIO;
    memcpy(disk[sector], in, 512);
    return 0;
}
int nos_block_flush(void) {
    return 0;
}
int nos_fat_mount(int format) {
    (void)format;
    return -NOS_ENOTSUP;
}
int nos_fat_list(const char *p, unsigned i, char *n, unsigned *m, size_t *s) {
    (void)p;
    (void)i;
    (void)n;
    (void)m;
    (void)s;
    return -NOS_ENOTSUP;
}
int nos_fat_read(const char *p, void *b, size_t n) {
    (void)p;
    (void)b;
    (void)n;
    return -NOS_ENOTSUP;
}
int nos_fat_write(const char *p, const void *b, size_t n) {
    (void)p;
    (void)b;
    (void)n;
    return -NOS_ENOTSUP;
}
int nos_fat_mkdir(const char *p) {
    (void)p;
    return -NOS_ENOTSUP;
}
int nos_fat_unlink(const char *p) {
    (void)p;
    return -NOS_ENOTSUP;
}
int nos_fat_rename(const char *p, const char *n) {
    (void)p;
    (void)n;
    return -NOS_ENOTSUP;
}
int nos_fat_sync(void) {
    fat_flushes++;
    return fat_error;
}
static intptr_t invoke(struct nos_message *m) {
    m->sender = m->process = 1;
    service(m);
    assert(!defer_reply);
    return m->value;
}
static intptr_t path(unsigned op, const char *name, unsigned flags) {
    struct nos_message m = {.op = op, .arg = flags, .len = strlen(name) + 1};
    assert(m.len <= NOS_MSG_DATA);
    memcpy(m.data, name, m.len);
    return invoke(&m);
}
static int open_file(const char *name) {
    return path(NOS_VFS_OPEN, name, NOS_O_CREAT | NOS_O_RDWR);
}
static intptr_t fd_op(unsigned op, int fd) {
    struct nos_message m = {.op = op, .arg = fd};
    return invoke(&m);
}
static intptr_t resize(int fd, uint64_t size) {
    struct nos_message m = {.op = NOS_VFS_FTRUNCATE, .arg = fd, .len = 8};
    memcpy(m.data, &size, 8);
    return invoke(&m);
}
static intptr_t write_bytes(int fd, unsigned char byte, size_t count) {
    struct nos_message m = {.op = NOS_VFS_WRITE, .arg = fd, .len = count};
    assert(count <= NOS_MSG_DATA);
    memset(m.data, byte, count);
    return invoke(&m);
}
static void close_file(int fd) {
    assert(!fd_op(NOS_VFS_CLOSE, fd));
}
static void remove_file(const char *name) {
    assert(!path(NOS_VFS_UNLINK, name, 0));
}
static void reset_namespace(void) {
    for (unsigned i = 0; i < NODE_COUNT; i++)
        clear_node(i);
    assert(!ram_backing_bytes && live_allocations == 2);
    memset(opened, 0, OPEN_COUNT * sizeof(*opened));
    memset(fds, 0, FD_TABLE_COUNT * sizeof(*fds));
    disk_mounted = fat_mounted = disk_generation = 0;
    mount_path[0] = fat_path[0] = 0;
    assert(!initialize_namespace());
}
static void grow_and_quota(void) {
    int fd = open_file("/tmp/grow");
    assert(fd == 3);
    for (unsigned i = 0; i < 8; i++)
        assert(write_bytes(fd, 0xa5, 64) == 64);
    fail_growth = 1;
    assert(write_bytes(fd, 1, 64) == -NOS_ENOMEM);
    unsigned node = opened[table_for(1)->fd[fd] - 1].node;
    assert(nodes[node].size == 512 && ram_backing_bytes == 0);
    for (unsigned i = 0; i < 512; i++)
        assert(node_bytes(&nodes[node])[i] == (char)0xa5);
    fail_growth = 0;
    assert(write_bytes(fd, 0x5a, 64) == 64 && ram_backing_bytes == 1024);
    unsigned live = live_allocations;
    fail_allocation = allocations + 3;
    assert(resize(fd, 8192) == -NOS_ENOMEM && ram_backing_bytes == 1024 &&
           live_allocations == live);
    assert(nodes[node].size == 576 && ram_backing[node].capacity == 1024);
    fail_allocation = 0;
    unsigned char content[8192];
    node_read_data(&nodes[node], 0, content, 576);
    for (unsigned i = 0; i < 512; i++)
        assert(content[i] == 0xa5);
    for (unsigned i = 512; i < 576; i++)
        assert(content[i] == 0x5a);
    assert(!resize(fd, 8192) && ram_backing_bytes == 8192);
    node_read_data(&nodes[node], 0, content, 8192);
    for (unsigned i = 576; i < 8192; i++)
        assert(!content[i]);
    assert(resize(fd, 8193) == -NOS_EFBIG && nodes[node].size == 8192);
    assert(!resize(fd, 513) && !resize(fd, 8192));
    node_read_data(&nodes[node], 0, content, 8192);
    for (unsigned i = 513; i < 8192; i++)
        assert(!content[i]);
    assert(!resize(fd, 512) && !ram_backing_bytes);
    close_file(fd);
    remove_file("/tmp/grow");
    char name[32];
    for (unsigned i = 0; i < 4; i++) {
        snprintf(name, sizeof(name), "/tmp/q%u", i);
        fd = open_file(name);
        assert(fd == 3 && !resize(fd, 8192));
        close_file(fd);
    }
    assert(ram_backing_bytes == RAM_BACKING_BYTES);
    fd = open_file("/tmp/extra");
    assert(fd == 3 && write_bytes(fd, 7, 1) == 1);
    assert(resize(fd, 513) == -NOS_ENOSPC);
    int pinned = open_file("/tmp/q0");
    assert(pinned == 4);
    struct nos_message dup = {.op = NOS_VFS_DUP, .arg = pinned, .value = -1};
    int alias = invoke(&dup);
    assert(alias == 5);
    remove_file("/tmp/q0");
    close_file(pinned);
    assert(ram_backing_bytes == RAM_BACKING_BYTES && resize(fd, 513) == -NOS_ENOSPC);
    close_file(alias);
    assert(!resize(fd, 8192));
    close_file(fd);
    fd = path(NOS_VFS_OPEN, "/tmp/extra", NOS_O_RDWR | NOS_O_TRUNC);
    assert(fd == 3 && ram_backing_bytes == 3 * 8192u);
    close_file(fd);
    remove_file("/tmp/extra");
    for (unsigned i = 1; i < 4; i++) {
        snprintf(name, sizeof(name), "/tmp/q%u", i);
        remove_file(name);
    }
    assert(!ram_backing_bytes && live_allocations == 3);
    puts("PASS esp_vfs_growth_failure_sparse_quota_unlink_dup_truncate_reclaim");
}
static void node_capacity_and_snapshot(void) {
    char name[32];
    for (unsigned i = 0; i < 26; i++) {
        snprintf(name, sizeof(name), "/tmp/n%u", i);
        int fd = open_file(name);
        assert(fd == 3);
        close_file(fd);
    }
    assert(open_file("/tmp/full") == -NOS_ENOSPC);
    for (unsigned i = 20; i < 26; i++) {
        snprintf(name, sizeof(name), "/tmp/n%u", i);
        remove_file(name);
    }
    assert(!disk_mount("/data"));
    int fd = open_file("/data/high");
    assert(fd == 3);
    assert(lookup("/data") >= 16 && lookup("/data/high") >= 16);
    assert(write_bytes(fd, 0x42, 64) == 64 && !fd_op(NOS_VFS_FSYNC, fd));
    close_file(fd);
    struct disk_header header;
    assert(!disk_bank_check(1, &header) && header.count == 2 && header.version == 2);
    /* The previous ESP profile serialized root/file at vnode indices 6/7,
     * with unused earlier slots. Accept that exact version-2 record layout. */
    memcpy(disk[33 + 13], disk[33 + 1], 1024);
    memcpy(disk[33 + 15], disk[33 + 3], 1024);
    memset(disk[33 + 1], 0, 4 * 512);
    header.crc = ~crc32(~0u, disk[34], 32 * 512);
    header.header_crc = ~crc32(~0u, &header, offsetof(struct disk_header, header_crc));
    memcpy(disk[33], &header, sizeof(header));
    reset_namespace();
    fail_allocation = allocations + 1;
    assert(disk_mount("/data") == -NOS_ENOMEM && lookup("/data") == -NOS_ENOENT);
    assert(live_allocations == 3 && !ram_backing_bytes && !disk_mounted);
    fail_allocation = 0;
    assert(!disk_mount("/data"));
    int index = lookup("/data/high");
    assert(index >= 0 && nodes[index].size == 64);
    for (unsigned i = 0; i < 64; i++)
        assert(nodes[index].bytes[i] == 0x42);
    fd = open_file("/data/high");
    assert(!resize(fd, 512) && resize(fd, 513) == -NOS_EFBIG);
    close_file(fd);
    for (unsigned i = 0; i < 14; i++) {
        snprintf(name, sizeof(name), "/data/n%u", i);
        fd = open_file(name);
        assert(fd == 3);
        close_file(fd);
    }
    unsigned saved = writes;
    assert(open_file("/data/overflow") == -NOS_ENOSPC && writes == saved);
    assert(!disk_sync());
    puts("PASS esp_vfs_32_nodes_high_slot_v2_snapshot_reload_and_admission");
}
static void backend_sync(void) {
    int ram = open_file("/tmp/ram"), persistent = open_file("/data/high");
    assert(ram == 3 && persistent == 4);
    unsigned saved = writes;
    fail_write = 1;
    assert(!fd_op(NOS_VFS_FSYNC, ram) && writes == saved);
    assert(fd_op(NOS_VFS_FSYNC, persistent) == -NOS_EIO && writes == saved + 1);
    fail_write = 0;
    assert(!fd_op(NOS_VFS_FSYNC, persistent));
    nodes[lookup("/tmp/ram")].persistent = 2;
    fat_error = -NOS_EIO;
    assert(fd_op(NOS_VFS_FSYNC, ram) == -NOS_EIO && fat_flushes == 1);
    fat_error = 0;
    assert(!fd_op(NOS_VFS_FSYNC, ram) && fat_flushes == 2);
    nodes[lookup("/tmp/ram")].persistent = 0;
    assert(fd_op(NOS_VFS_FSYNC, -1) == -NOS_EBADF && fd_op(NOS_VFS_FSYNC, 0) == -NOS_EINVAL);
    int proc = path(NOS_VFS_OPEN, "/proc/version", NOS_O_RDONLY);
    assert(proc >= 3 && fd_op(NOS_VFS_FSYNC, proc) == -NOS_EINVAL);
    close_file(proc);
    struct nos_message pipe = {.op = NOS_VFS_PIPE};
    assert(!invoke(&pipe));
    int pair[2];
    memcpy(pair, pipe.data, sizeof(pair));
    assert(fd_op(NOS_VFS_FSYNC, pair[0]) == -NOS_EINVAL);
    close_file(pair[0]);
    close_file(pair[1]);
    close_file(ram);
    close_file(persistent);
    puts("PASS vfs_fsync_selects_fd_backend_and_preserves_errors");
}
int main(void) {
    for (unsigned i = 1; i <= 3; i++) {
        allocations = 0;
        fail_allocation = i;
        assert(initialize_namespace() == -NOS_ENOMEM && !opened && !nodes && !fds &&
               !live_allocations);
    }
    fail_allocation = 0;
    assert(!initialize_namespace());
    /* Model fragmented heap: all file backing allocations above one sector
     * fail, while tables were allocated during initialization. */
    max_allocation = 512;
    assert(NODE_COUNT == 32 && FILE_BYTES == 8192);
    grow_and_quota();
    node_capacity_and_snapshot();
    backend_sync();
    reset_namespace();
    for (unsigned i = 0; i < NODE_COUNT; i++)
        clear_node(i);
    nos_vfs_free(nodes);
    nos_vfs_free(opened);
    nodes = NULL;
    opened = NULL;
    fds = NULL;
    assert(!live_allocations);
    puts("PASS esp_vfs_allocation_rollback_and_no_leaks");
    return 0;
}
