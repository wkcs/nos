/* Copyright (C) 2026 NOS contributors. Native VFS uses NOS-managed devices. */
#include "nos/driver/esp_idf.h"
#include "../../../../servers/drivers/devices.h"
#include "../../../../servers/fatfs/adapter.h"
#include "esp_heap_caps.h"

void *nos_vfs_allocate(size_t bytes) {
    return heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}
void nos_vfs_free(void *address) {
    heap_caps_free(address);
}

void nos_uart_init(void) {
    /* UART0 is already managed by the system console. No second RX driver. */
}
intptr_t nos_device_uart_read(void) {
    unsigned char byte;
    struct nos_io_buffer b = {&byte, 1, 0};
    int rc = nos_idf_console_call(NOS_IO_READ, &b, sizeof(b));
    return rc ? rc : b.actual == 1 ? byte : -NOS_EAGAIN;
}
intptr_t nos_device_uart_write(const void *buffer, size_t count) {
    struct nos_io_buffer b = {(void *)buffer, count, 0};
    int rc = nos_idf_console_call(NOS_IO_WRITE, &b, sizeof(b));
    return rc ? rc : (intptr_t)b.actual;
}
int nos_uart_readable(void) {
    struct nos_io_console_status status = {0};
    return !nos_idf_console_call(NOS_IO_POLL, &status, sizeof(status)) && status.readable;
}
int nos_uart_writable(void) {
    struct nos_io_console_status status = {0};
    return !nos_idf_console_call(NOS_IO_POLL, &status, sizeof(status)) && status.writable;
}
void nos_uart_get_stats(struct nos_uart_stats *stats) {
    *stats = (struct nos_uart_stats){0};
}
void nos_esp_vfs_entry(void *argument) {
    extern void nos_vfs_server(uintptr_t argument);
    nos_vfs_server((uintptr_t)argument);
}

/* Entropy and FatFs are not bound in this profile. The native two-bank
 * filesystem uses the separately managed dedicated NOR block service. */
intptr_t nos_random_read(void *buffer, size_t count, unsigned flags) {
    (void)buffer;
    (void)count;
    (void)flags;
    return -NOS_ENOTSUP;
}
int nos_fat_mount(int format) {
    (void)format;
    return -NOS_ENOTSUP;
}
int nos_fat_list(const char *path, unsigned index, char *name, unsigned *mode, size_t *size) {
    (void)path;
    (void)index;
    (void)name;
    (void)mode;
    (void)size;
    return -NOS_ENOTSUP;
}
int nos_fat_read(const char *path, void *buffer, size_t size) {
    (void)path;
    (void)buffer;
    (void)size;
    return -NOS_ENOTSUP;
}
int nos_fat_write(const char *path, const void *buffer, size_t size) {
    (void)path;
    (void)buffer;
    (void)size;
    return -NOS_ENOTSUP;
}
int nos_fat_mkdir(const char *path) {
    (void)path;
    return -NOS_ENOTSUP;
}
int nos_fat_unlink(const char *path) {
    (void)path;
    return -NOS_ENOTSUP;
}
int nos_fat_rename(const char *old, const char *next) {
    (void)old;
    (void)next;
    return -NOS_ENOTSUP;
}
int nos_fat_sync(void) {
    return -NOS_ENOTSUP;
}
