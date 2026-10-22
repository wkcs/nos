/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
#include "user.h"

/* Native process entry, libc thread state, and termination. */
#ifdef NOS_NEWLIB
#include <reent.h>
#include <string.h>
#endif

__attribute__((weak)) void nos_libc_task_cleanup(void)
{
}

void nos_process_exit(int status)
{
    nos_libc_task_cleanup();
    nos_syscall(NOS_SYS_EXIT, (uintptr_t)status, 0, 0);
    for (;;) {
    }
}

void nos_idle(uintptr_t unused)
{
    (void)unused;
    for (;;)
        nos_syscall(NOS_SYS_YIELD, 0, 0, 0);
}

void nos_process_start(uintptr_t entry, uintptr_t argument)
{
#ifdef NOS_NEWLIB
    extern void nos_newlib_init(void);
    nos_newlib_init();
    _Static_assert(sizeof(struct _reent) <= NOS_PAGE_SIZE, "Newlib thread storage is too large");
    struct _reent local_reent;
    struct _reent *reent = &local_reent;
    _REENT_INIT_PTR(reent);
#ifdef NOS_NEWLIB_DYNAMIC
    extern int nos_newlib_bind_reent(struct _reent *);
    if (nos_newlib_bind_reent(reent))
#else
    if (nos_syscall(NOS_SYS_TLS_BIND, (uintptr_t)&_impure_ptr, (uintptr_t)reent, 0))
#endif
        nos_syscall(NOS_SYS_EXIT, 1, 0, 0);
#endif
#ifdef NOS_MUSL
    static uintptr_t environment[] = { 0, 6, NOS_PAGE_SIZE, 0, 0 };
    extern void __init_libc(char **environment, char *name);
    __init_libc((char **)environment, "nos");
#endif
    ((void (*)(uintptr_t))entry)(argument);
    nos_process_exit(0);
}
