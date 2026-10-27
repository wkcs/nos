/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * NOS User-Space / Service IPC Client Library (libnos)
 */

#include <nos/syscall_arch.h>
#include <nos/user_ipc.h>
#include <string.h>

#define NOS_CAP_OP_CREATE_EP    10U
#define NOS_CAP_OP_CREATE_NOTIF 11U

int nos_user_ipc_call(nos_handle_t ep_cap,
                      uint16_t opcode,
                      uint64_t r0,
                      uint64_t r1,
                      uint64_t r2,
                      uint64_t r3,
                      const void *buf,
                      uint32_t len,
                      struct nos_ipc_msg *out_resp)
{
    struct nos_ipc_msg req;
    struct nos_ipc_msg resp;
    long rc;

    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));

    req.tag = NOS_IPC_TAG_MAKE(opcode, NOS_IPC_FLAG_NONE, 0U);
    req.payload_len = len;
    req.regs[0] = r0;
    req.regs[1] = r1;
    req.regs[2] = r2;
    req.regs[3] = r3;
    req.buf_addr = (uint64_t)(uintptr_t)buf;

    rc = nos_syscall4(SYS_NOS_IPC_CALL,
                      (uintptr_t)ep_cap,
                      (uintptr_t)&req,
                      (uintptr_t)&resp,
                      (uintptr_t)NOS_IPC_TIMEOUT_INFINITE);
    if (out_resp != NULL) {
        *out_resp = resp;
    }
    return (int)rc;
}

int nos_user_ipc_recv(nos_handle_t ep_cap,
                      struct nos_ipc_msg *msg,
                      uint32_t timeout_ticks)
{
    return (int)nos_syscall3(SYS_NOS_IPC_RECV,
                             (uintptr_t)ep_cap,
                             (uintptr_t)msg,
                             (uintptr_t)timeout_ticks);
}

int nos_user_ipc_reply_wait(nos_handle_t ep_cap,
                            const struct nos_ipc_msg *reply,
                            struct nos_ipc_msg *next_req,
                            uint32_t timeout_ticks)
{
    return (int)nos_syscall4(SYS_NOS_IPC_REPLY_WAIT,
                             (uintptr_t)ep_cap,
                             (uintptr_t)reply,
                             (uintptr_t)next_req,
                             (uintptr_t)timeout_ticks);
}

nos_handle_t nos_user_endpoint_create(void)
{
    nos_handle_t h = NOS_HANDLE_INVALID;
    long rc = nos_syscall5(SYS_NOS_CAP_CONTROL,
                           NOS_CAP_OP_CREATE_EP,
                           0U,
                           0U,
                           0U,
                           (uintptr_t)&h);
    return (rc < 0) ? NOS_HANDLE_INVALID : h;
}

nos_handle_t nos_user_notification_create(void)
{
    nos_handle_t h = NOS_HANDLE_INVALID;
    long rc = nos_syscall5(SYS_NOS_CAP_CONTROL,
                           NOS_CAP_OP_CREATE_NOTIF,
                           0U,
                           0U,
                           0U,
                           (uintptr_t)&h);
    return (rc < 0) ? NOS_HANDLE_INVALID : h;
}
