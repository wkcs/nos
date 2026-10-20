/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_ELF_H
#define NOS_ELF_H
#include "init.h"
struct task;
struct nos_elf_result { uintptr_t entry, stack, argv, tls; unsigned argc, abi; uintptr_t gateway_entry; };
int nos_exec_copy_args(const struct nos_exec_request *request, struct nos_exec_args *args);
int nos_elf_load(struct task *task, const void *image, size_t bytes,
                 const struct nos_exec_args *args, struct nos_elf_result *result);
int nos_elf_load_linux(struct task *task, const void *image, size_t bytes,
                      const struct nos_exec_args *args, const struct nos_credentials *credentials,
                      struct nos_elf_result *result);
const void *nos_initramfs_find(const char *path, size_t *bytes);
const char *nos_initramfs_entry(size_t index, size_t *bytes);
#endif
