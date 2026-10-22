/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
#include "user.h"

/* Synchronous VFS request/reply transport. */

intptr_t nos_rpc(struct nos_message *m)
{
    intptr_t rc = nos_syscall(NOS_SYS_CALL, 0, (uintptr_t)m, (uintptr_t)m);
    return rc < 0 ? rc : m->value;
}
