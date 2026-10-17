/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
#include "elf.h"
static int equal_path(const char *a, const char *b)
{
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
intptr_t bootfile_operation(uintptr_t operation, uintptr_t pointer)
{
    if (!(current_task->caps & NOS_CAP_DEVICE)) return -NOS_EACCES;
    struct nos_bootfile_io request;
    if (user_copy_in(&request, pointer, sizeof(request))) return -NOS_EFAULT;
    if (request.flags) return -NOS_EINVAL;
    if (operation == NOS_BOOTFILE_LOOKUP) {
        char path[NOS_PATH_MAX]; unsigned length;
        for (length = 0; length < sizeof(path); length++) {
            if (request.buffer > UINTPTR_MAX - length) return -NOS_EFAULT;
            if (user_copy_in(path + length, request.buffer + length, 1)) return -NOS_EFAULT;
            if (!path[length]) break;
        }
        if (length == sizeof(path)) return -NOS_ENAMETOOLONG;
        for (size_t i = 0;; i++) {
            size_t bytes; const char *name = nos_initramfs_entry(i, &bytes);
            if (!name) return -NOS_ENOENT;
            if (equal_path(name, path)) return (intptr_t)i;
        }
    }
    size_t bytes; const char *name = nos_initramfs_entry(request.index, &bytes);
    if (!name) return -NOS_ENOENT;
    if (operation == NOS_BOOTFILE_ENUM) {
        size_t length = 0; while (name[length]) length++;
        if (length >= NOS_PATH_MAX) return -NOS_ENAMETOOLONG;
        if (length + 1 > request.count) return -NOS_ERANGE;
        int rc = user_copy_out(request.buffer, name, length + 1);
        return rc ? rc : (intptr_t)(length + 1);
    }
    if (operation == NOS_BOOTFILE_STAT) {
        struct nos_stat64 stat;
        k_memset(&stat, 0, sizeof(stat)); stat.ino = 0x1000u + request.index;
        stat.size = bytes; stat.blocks = (bytes + 511) / 512;
        stat.mode = 0100444; stat.nlink = 1;
        return user_copy_out(request.buffer, &stat, sizeof(stat));
    }
    if (operation != NOS_BOOTFILE_READ || request.count > 4096) return -NOS_EINVAL;
    if (request.offset >= bytes) return 0;
    size_t count = request.count;
    if (count > bytes - request.offset) count = bytes - (size_t)request.offset;
    const unsigned char *image = nos_initramfs_find(name, &bytes);
    if (!image) return -NOS_EIO;
    int rc = user_copy_out(request.buffer, image + (size_t)request.offset, count);
    return rc ? rc : (intptr_t)count;
}
