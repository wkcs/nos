/* Copyright (C) 2026 NOS contributors */
#include "user.h"
#include <stdlib.h>
#ifdef NOS_NEWLIB
#include <reent.h>
#include <string.h>
static struct _reent app_reent;
#endif
void nos_app_libc_init(int argc, char **argv)
{
#ifdef NOS_MUSL
    extern void __init_libc(char **environment, char *name);
    __init_libc(argv + argc + 1, argv[0]);
#elif defined(NOS_NEWLIB)
    (void)argc; (void)argv;
    extern void nos_newlib_init(void);
    nos_newlib_init();
    _REENT_INIT_PTR(&app_reent);
#ifdef NOS_NEWLIB_DYNAMIC
    extern int nos_newlib_bind_reent(struct _reent *);
    if (nos_newlib_bind_reent(&app_reent))
#else
    if (nos_syscall(NOS_SYS_TLS_BIND, (uintptr_t)&_impure_ptr, (uintptr_t)&app_reent, 0))
#endif
        nos_syscall(NOS_SYS_EXIT, 97, 0, 0);
#else
    (void)argc; (void)argv;
#endif
}
void nos_app_finish(int status)
{
    exit(status);
    for (;;) { }
}
