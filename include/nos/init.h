/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_INIT_H
#define NOS_INIT_H
#include "abi.h"
#ifdef NOS_LINUX_COMPAT
#define NOS_EXEC_BYTES 131072u
#define NOS_EXEC_ARGC (NOS_EXEC_BYTES / (sizeof(uintptr_t) + 1u))
#define NOS_EXEC_STRLEN 256u
#else
#define NOS_EXEC_ARGC 8u
#define NOS_EXEC_STRLEN 64u
#endif
#define NOS_AUX_TLS 0x6000u
struct nos_exec_args {
    unsigned argc, envc;
    char path[NOS_EXEC_STRLEN];
#ifdef NOS_LINUX_COMPAT
    unsigned used;
    uint32_t offsets[NOS_EXEC_ARGC];
    char strings[NOS_EXEC_BYTES];
#else
    char argv[NOS_EXEC_ARGC][NOS_EXEC_STRLEN];
    char envp[NOS_EXEC_ARGC][NOS_EXEC_STRLEN];
#endif
    uint8_t random[16];
    unsigned random_valid;
};
struct nos_tls_image { uintptr_t base; size_t filesz, memsz, align; };
void nos_init(uintptr_t argument);
int nos_exec(const char *path, const char *const *argv, const char *const *envp, unsigned priority);
#endif
