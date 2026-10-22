/* Copyright (C) 2026 NOS contributors */
#include "abi.h"
#include "user.h"

/* Process credential syscall wrappers. */

int nos_credentials_get(struct nos_credentials *credentials)
{
#ifdef NOS_LINUX_COMPAT
    return (int)nos_syscall(NOS_SYS_CREDENTIALS, NOS_CRED_GET, (uintptr_t)credentials, 0);
#else
    (void)credentials;
    return -NOS_ENOTSUP;
#endif
}

int nos_credentials_set(unsigned command, const struct nos_credential_request *request)
{
#ifdef NOS_LINUX_COMPAT
    if (command == NOS_CRED_GET || command > NOS_CRED_SETGROUPS)
        return -NOS_EINVAL;
    return (int)nos_syscall(NOS_SYS_CREDENTIALS, command, (uintptr_t)request, 0);
#else
    (void)command;
    (void)request;
    return -NOS_ENOTSUP;
#endif
}
