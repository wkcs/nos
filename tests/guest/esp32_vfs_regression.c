/* Copyright (C) 2026 NOS contributors. Actual native VFS/IPC on C3 and S3. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "nos/user.h"
#include "esp_idf_test.h"
#include "esp_heap_caps.h"

static SemaphoreHandle_t finished;
static int parent_fd;
static void client(void *argument) {
    (void)argument;
    char cwd[NOS_PATH_MAX], data[32];
    struct nos_message request = {0};
    assert(nos_syscall(NOS_SYS_RECV, 0, (uintptr_t)&request, 0) == -NOS_EACCES);
    assert(nos_getcwd(cwd, sizeof(cwd)) >= 0 && !strcmp(cwd, "/"));
    assert(nos_read(parent_fd, data, 1) == -NOS_EBADF);
    int fd = nos_open("/proc/version", NOS_O_RDONLY);
    assert(fd >= 3 && nos_read(fd, data, sizeof(data)) == 16);
    assert(!memcmp(data, "NOS microkernel\n", 16));
    fd = nos_open("/tmp/exit-owned", NOS_O_CREAT | NOS_O_RDWR);
    assert(fd == 4 && !nos_ftruncate(fd, 8192) && !nos_unlink("/tmp/exit-owned"));
    /* Deliberately leave the FD open: native exit must notify VFS and reclaim
     * the process table; SDK task records/stack are reclaimed by NOS idle. */
    xSemaphoreGive(finished);
    nos_syscall(NOS_SYS_EXIT, 0, 0, 0);
    assert(0);
}
void nos_esp_vfs_regression(void) {
    assert(!nos_mkdir("/tmp/vfstest"));
    assert(!nos_chdir("/tmp/vfstest"));
    char cwd[NOS_PATH_MAX];
    assert(nos_getcwd(cwd, sizeof(cwd)) >= 0 && !strcmp(cwd, "/tmp/vfstest"));
    int fd = nos_open("note", NOS_O_CREAT | NOS_O_RDWR | NOS_O_TRUNC);
    assert(fd == 3);
    const char payload[] = "native IPC VFS";
    assert(nos_write(fd, payload, sizeof(payload)) == sizeof(payload));
    assert(nos_seek(fd, 0, 0) == 0);
    char data[512];
    assert(nos_read(fd, data, sizeof(data)) == sizeof(payload));
    assert(!memcmp(data, payload, sizeof(payload)));
    assert(nos_read(fd, (void *)1, 1) == -NOS_EFAULT);
    int alias = nos_dup(fd);
    assert(alias >= 4 && nos_seek(alias, 0, 0) == 0);
    assert(nos_read(fd, data, 1) == 1 && data[0] == 'n');
    assert(nos_seek(alias, 0, 1) == 1);
    assert(!nos_rename("note", "renamed"));
    assert(!nos_unlink("renamed"));
    assert(nos_seek(fd, 0, 0) == 0 && nos_read(fd, data, sizeof(data)) == sizeof(payload));
    assert(!nos_close(alias) && !nos_close(fd));
    assert(nos_open("renamed", NOS_O_RDONLY) == -NOS_ENOENT);
    assert(nos_open("/proc/version", NOS_O_WRONLY) == -NOS_EACCES);
    puts("PASS " NOS_IDF_TEST_NAME "_vfs_files_cwd_dup_unlink_permissions");

    int pipe[2];
    assert(!nos_pipe(pipe));
    assert(nos_write(pipe[1], payload, sizeof(payload)) == sizeof(payload));
    assert(nos_read(pipe[0], data, sizeof(data)) == sizeof(payload));
    assert(!memcmp(data, payload, sizeof(payload)));
    assert(!nos_close(pipe[1]) && nos_read(pipe[0], data, 1) == 0);
    assert(!nos_close(pipe[0]));
    assert(!nos_pipe(pipe));
    assert(!nos_close(pipe[0]) && nos_write(pipe[1], payload, 1) == -32);
    assert(!nos_close(pipe[1]));
    puts("PASS " NOS_IDF_TEST_NAME "_vfs_pipe_eof_epipe");

    int descriptors[NOS_FD_LIMIT];
    unsigned count = 0;
    while ((fd = nos_open("/proc/version", NOS_O_RDONLY)) >= 0) {
        assert(count < NOS_FD_LIMIT);
        descriptors[count++] = fd;
    }
    assert(fd == -NOS_EMFILE && count == NOS_FD_LIMIT - 3);
    while (count)
        assert(!nos_close(descriptors[--count]));
    fd = nos_open("full", NOS_O_CREAT | NOS_O_RDWR);
    assert(fd >= 3);
    memset(data, 0x5a, sizeof(data));
    for (unsigned i = 0; i < 16; i++)
        assert(nos_write(fd, data, sizeof(data)) == sizeof(data));
    assert(nos_write(fd, data, 1) == -NOS_ENOSPC);
    struct nos_stat st;
    assert(!nos_stat_fd(fd, &st) && st.size == 8192);
    assert(nos_ftruncate(fd, 8193) == -NOS_EFBIG);
    assert(!nos_ftruncate(fd, 513) && !nos_ftruncate(fd, 8192));
    assert(nos_pread(fd, data, sizeof(data), 513) == sizeof(data));
    for (unsigned i = 0; i < sizeof(data); i++)
        assert(data[i] == 0);
    assert(!nos_fsync(fd));
    assert(!nos_close(fd) && !nos_unlink("full"));
    puts("PASS " NOS_IDF_TEST_NAME "_vfs_descriptor_capacity_file_bounds");

    char name[32];
    for (unsigned cycle = 0; cycle < 2; cycle++) {
        for (unsigned i = 0; i < 4; i++) {
            snprintf(name, sizeof(name), "quota-%u", i);
            fd = nos_open(name, NOS_O_CREAT | NOS_O_RDWR);
            int grown = fd >= 3 ? nos_ftruncate(fd, 8192) : fd;
            if (fd != 3 || grown)
                printf("RAM quota cycle=%u file=%u fd=%d grow=%d heap=%u largest=%u\n", cycle, i,
                       fd, grown, (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                       (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
            assert(fd == 3 && !grown && !nos_close(fd));
        }
        fd = nos_open("quota-extra", NOS_O_CREAT | NOS_O_RDWR);
        assert(fd == 3 && nos_write(fd, "keep", 4) == 4);
        assert(nos_ftruncate(fd, 513) == -NOS_ENOSPC);
        int pinned = nos_open("quota-0", NOS_O_RDONLY), alias = nos_dup(pinned);
        assert(pinned == 4 && alias == 5 && !nos_unlink("quota-0") && !nos_close(pinned));
        assert(nos_ftruncate(fd, 513) == -NOS_ENOSPC);
        assert(!nos_close(alias) && !nos_ftruncate(fd, 8192));
        assert(nos_pread(fd, data, 4, 0) == 4 && !memcmp(data, "keep", 4));
        assert(!nos_close(fd));
        fd = nos_open("quota-extra", NOS_O_RDWR | NOS_O_TRUNC);
        assert(fd == 3 && !nos_close(fd) && !nos_unlink("quota-extra"));
        for (unsigned i = 1; i < 4; i++) {
            snprintf(name, sizeof(name), "quota-%u", i);
            assert(!nos_unlink(name));
        }
    }
    puts("PASS " NOS_IDF_TEST_NAME "_ramfs_quota_sparse_truncate_unlink_reclaim");
    for (unsigned i = 0; i < 25; i++) {
        snprintf(name, sizeof(name), "node-%u", i);
        fd = nos_open(name, NOS_O_CREAT | NOS_O_RDWR);
        assert(fd == 3 && !nos_close(fd));
    }
    assert(nos_open("node-full", NOS_O_CREAT | NOS_O_RDWR) == -NOS_ENOSPC);
    for (unsigned i = 0; i < 25; i++) {
        snprintf(name, sizeof(name), "node-%u", i);
        assert(!nos_unlink(name));
    }
    puts("PASS " NOS_IDF_TEST_NAME "_ramfs_32_node_admission_recovery");

    parent_fd = nos_open("/proc/version", NOS_O_RDONLY);
    assert(parent_fd == 3);
    finished = xSemaphoreCreateBinary();
    assert(finished);
    for (unsigned i = 0; i < NOS_PROCESSES + 8; i++) {
        assert(xTaskCreate(client, "vfs-client", 4096, NULL, 1, NULL) == pdPASS);
        assert(xSemaphoreTake(finished, 100) == pdTRUE);
        vTaskDelay(2);
    }
    vSemaphoreDelete(finished);
    assert(!nos_close(parent_fd));
    assert(!nos_chdir("/") && !nos_rmdir("/tmp/vfstest"));
    TaskHandle_t service = xTaskGetHandle("vfs");
    assert(service && uxTaskGetStackHighWaterMark(service) >= 512);
    printf("VFS stack_free=%u\n", (unsigned)uxTaskGetStackHighWaterMark(service));
    puts("PASS " NOS_IDF_TEST_NAME "_vfs_task_rights_exit_fd_reclamation");
}
