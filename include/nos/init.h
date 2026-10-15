/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_INIT_H
#define NOS_INIT_H
#include "abi.h"
#define NOS_EXEC_ARGC 8u
#define NOS_EXEC_STRLEN 64u
#define NOS_AUX_TLS 0x6000u
struct nos_exec_args {
    unsigned argc, envc;
    char path[NOS_EXEC_STRLEN];
    char argv[NOS_EXEC_ARGC][NOS_EXEC_STRLEN];
    char envp[NOS_EXEC_ARGC][NOS_EXEC_STRLEN];
};
struct nos_tls_image { uintptr_t base; size_t filesz, memsz, align; };
void nos_init(uintptr_t argument);
int nos_exec(const char *path, const char *const *argv, const char *const *envp, unsigned priority);
#endif
