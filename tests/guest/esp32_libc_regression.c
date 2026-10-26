/* Copyright (C) 2026 NOS contributors. Real pinned Newlib calls, C3 and S3. */
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "esp_vfs.h"
#include "nos/user.h"
#include "esp_idf_test.h"

static SemaphoreHandle_t ready;
static FILE *parent_file, *parent_stdout;
static DIR *parent_directory;
static int parent_fd;
static unsigned completed;
static void capacity_client(void *argument) {
    (void)argument;
    assert(stdout != parent_stdout && fileno(stdout) == 1);
    xSemaphoreGive(ready);
    vTaskSuspend(NULL);
    assert(0);
}
static void owner_client(void *argument) {
    (void)argument;
    assert(stdout != parent_stdout);
    errno = 0;
    assert(fclose(parent_file) == EOF && errno == EBADF);
    assert(closedir(parent_directory) == -1 && errno == EBADF);
    char byte;
    assert(read(parent_fd, &byte, 1) == -1 && errno == EBADF);
    FILE *file = fopen("/tmp/libc-child", "w");
    assert(file && fileno(file) == parent_fd);
    assert(fwrite("flushed", 1, 7, file) == 7);
    assert(!fclose(stdout));
    completed++;
    xSemaphoreGive(ready);
    /* Normal task return flushes/closes its remaining FILE objects. */
}
static void orphan_client(void *argument) {
    FILE *file = fopen("/tmp/libc-orphan", "w");
    assert(file && fwrite("discard", 1, 7, file) == 7);
    assert(opendir("/tmp"));
    completed++;
    xSemaphoreGive(ready);
    if (argument) {
        vTaskSuspend(NULL);
        assert(0);
    }
    nos_syscall(NOS_SYS_EXIT, 0, 0, 0);
    assert(0);
}
static ssize_t sdk_read(int fd, void *buffer, size_t size) {
    assert(fd == 7);
    size_t n = size < 3 ? size : 3;
    memcpy(buffer, "SDK", n);
    return n;
}
static int sdk_open(const char *path, int flags, int mode) {
    (void)path;
    (void)flags;
    (void)mode;
    return 7;
}
static int sdk_close(int fd) {
    assert(fd == 7);
    return 0;
}
void nos_esp_libc_regression(void) {
    assert(!mkdir("/tmp/libc", 0755) && !chdir("/tmp/libc"));
    char data[128], path[NOS_PATH_MAX];
    assert(getcwd(path, sizeof(path)) && !strcmp(path, "/tmp/libc"));
    FILE *file = fopen("note", "w+");
    assert(file && fileno(file) == 3);
    assert(fprintf(file, "number=%d\n", 42) == 10 && !fflush(file));
    assert(nos_seek(fileno(file), 0, SEEK_SET) == 0);
    assert(nos_read(fileno(file), data, sizeof(data)) == 10 && !memcmp(data, "number=42\n", 10));
    assert(!fseek(file, 0, SEEK_SET));
    int number;
    assert(fscanf(file, "number=%d", &number) == 1 && number == 42);
    assert(freopen("note", "a+", file) == file);
    assert(fputs("tail\n", file) >= 0 && !fclose(file));
    struct stat st;
    assert(!stat("note", &st) && S_ISREG(st.st_mode) && st.st_size == 15);
    int fd = open("note", O_RDWR);
    assert(fd == 3 && !fsync(fd));
    assert(fsync(-1) == -1 && errno == EBADF);
    assert(fsync(1) == -1 && errno == EINVAL);
    assert(!isatty(fd) && errno == ENOTTY && isatty(1));
    assert(pread(fd, data, 6, 0) == 6 && !memcmp(data, "number", 6));
    assert(pwrite(fd, "N", 1, 0) == 1 && pread(fd, data, 1, 0) == 1 && data[0] == 'N');
    assert(pwrite(fd, "n", 1, 0) == 1);
    assert(lseek(fd, 0, SEEK_CUR) == 0);
    assert(!fcntl(fd, F_SETFD, FD_CLOEXEC) && fcntl(fd, F_GETFD) == FD_CLOEXEC);
    int alias = dup(fd);
    assert(alias == 4 && lseek(alias, 0, SEEK_SET) == 0);
    assert(read(fd, data, 1) == 1 && data[0] == 'n' && lseek(alias, 0, SEEK_CUR) == 1);
    assert(!close(alias) && !ftruncate(fd, 6) && !fstat(fd, &st) && st.st_size == 6);
    assert(!close(fd) && !rename("note", "renamed"));
    DIR *directory = opendir(".");
    assert(directory);
    int found = 0;
    struct dirent *entry;
    while ((entry = readdir(directory)))
        found |= !strcmp(entry->d_name, "renamed");
    assert(found);
    rewinddir(directory);
    struct dirent copy, *result;
    assert(!readdir_r(directory, &copy, &result) && result == &copy);
    assert(telldir(directory) == 1);
    assert(!closedir(directory) && !unlink("renamed") && !chdir("/") && !rmdir("/tmp/libc"));
    errno = 0;
    assert(!fopen("/no-such-file", "r") && errno == ENOENT);
    assert(!fopen("/proc/version", "w") && errno == EACCES);
    assert(open("/tmp/unsupported", O_CREAT | O_WRONLY | O_SYNC, 0600) == -1 && errno == ENOTSUP);
    fd = open("/proc/version", O_RDONLY);
    assert(fd == 3);
    FILE *aliases[NOS_FD_LIMIT + 3];
    unsigned aliases_count = 0;
    while ((file = fdopen(fd, "r"))) {
        assert(aliases_count < NOS_FD_LIMIT + 3);
        aliases[aliases_count++] = file;
    }
    assert(errno == EMFILE && fcntl(fd, F_GETFD) >= 0);
    while (aliases_count)
        fclose(aliases[--aliases_count]);
    puts("PASS " NOS_IDF_TEST_NAME "_newlib_stdio_native_fd_flags_directories_errno");

    int pipefd[2];
    assert(!pipe(pipefd));
    struct pollfd pollfd = {pipefd[0], POLLIN, 0};
    assert(!poll(&pollfd, 1, 0));
    assert(write(pipefd[1], "p", 1) == 1);
    pollfd.events = POLLIN | POLLRDNORM;
    assert(poll(&pollfd, 1, 0) == 1 && (pollfd.revents & POLLIN) && (pollfd.revents & POLLRDNORM));
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(pipefd[0], &read_set);
    struct timeval zero = {0};
    assert(select(pipefd[0] + 1, &read_set, NULL, NULL, &zero) == 1 &&
           FD_ISSET(pipefd[0], &read_set));
    assert(read(pipefd[0], data, 1) == 1 && data[0] == 'p');
    assert(!close(pipefd[1]));
    assert(poll(&pollfd, 1, 0) == 1 && (pollfd.revents & POLLHUP));
    assert(!close(pipefd[0]));
    assert(poll(&pollfd, 1, 0) == 1 && (pollfd.revents & POLLNVAL));
    puts("PASS " NOS_IDF_TEST_NAME "_newlib_pipe_poll_select_abi_translation");

    const esp_vfs_t sdk = {
        .flags = ESP_VFS_FLAG_DEFAULT, .open = sdk_open, .read = sdk_read, .close = sdk_close};
    assert(!esp_vfs_register("/sdk-libc-test", &sdk, NULL));
    fd = esp_vfs_open(__getreent(), "/sdk-libc-test/data", O_RDONLY, 0);
    assert(fd >= (int)NOS_FD_LIMIT && read(fd, data, 3) == 3 && !memcmp(data, "SDK", 3));
    int native = open("/proc/version", O_RDONLY);
    assert(native == 3 && read(native, data, 3) == 3 && !memcmp(data, "NOS", 3));
    assert(!close(fd) && !close(native) && !esp_vfs_unregister("/sdk-libc-test"));
    puts("PASS " NOS_IDF_TEST_NAME "_newlib_sdk_fd_range_coexistence");

    parent_file = fopen("/tmp/libc-parent", "w+");
    assert(parent_file);
    parent_fd = fileno(parent_file);
    assert(parent_fd == 3);
    parent_directory = opendir("/tmp");
    parent_stdout = stdout;
    ready = xSemaphoreCreateBinary();
    assert(parent_directory && ready);
    assert(xTaskCreate(owner_client, "libc-owner", 6144, NULL, 1, NULL) == pdPASS);
    assert(xSemaphoreTake(ready, 100) == pdTRUE);
    vTaskDelay(3);
    assert(write(parent_fd, "parent", 6) == 6 && !fstat(parent_fd, &st));
    assert(!stat("/tmp/libc-child", &st) && st.st_size == 7);
    /* Prime Newlib's global FILE slot pool before the exact leak check. */
    assert(xTaskCreate(orphan_client, "libc-prime", 6144, NULL, 1, NULL) == pdPASS);
    assert(xSemaphoreTake(ready, 100) == pdTRUE);
    vTaskDelay(3);
    size_t before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    for (unsigned i = 0; i < NOS_PROCESSES + 8; i++) {
        TaskHandle_t task;
        assert(xTaskCreate(orphan_client, "libc-orphan", 6144, (void *)(uintptr_t)(i & 1), 1,
                           &task) == pdPASS);
        assert(xSemaphoreTake(ready, 100) == pdTRUE);
        if (i & 1)
            vTaskDelete(task);
        vTaskDelay(3);
        assert(!stat("/tmp/libc-orphan", &st) && !st.st_size);
        assert(fcntl(parent_fd, F_GETFD) >= 0);
    }
    assert(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == before);
    assert(!fclose(parent_file) && !closedir(parent_directory));
    vSemaphoreDelete(ready);
    assert(!unlink("/tmp/libc-parent") && !unlink("/tmp/libc-child") &&
           !unlink("/tmp/libc-orphan"));
    puts("PASS " NOS_IDF_TEST_NAME "_newlib_stdio_task_ownership_exit_reclamation");
    ready = xSemaphoreCreateCounting(NOS_TASKS, 0);
    assert(ready);
    unsigned baseline = uxTaskGetNumberOfTasks(), count = 0;
    TaskHandle_t tasks[NOS_TASKS];
    while (xTaskCreate(capacity_client, "libc-capacity", 2048, NULL, 1, &tasks[count]) == pdPASS) {
        count++;
        assert(count < NOS_TASKS);
    }
    assert(count + baseline == NOS_TASKS);
    for (unsigned i = 0; i < count; i++)
        assert(xSemaphoreTake(ready, 100) == pdTRUE);
    for (unsigned i = 0; i < count; i++)
        vTaskDelete(tasks[i]);
    vSemaphoreDelete(ready);
    vTaskDelay(3);
    assert(uxTaskGetNumberOfTasks() == baseline);
    puts("PASS " NOS_IDF_TEST_NAME "_newlib_private_stdio_full_task_capacity");
}
