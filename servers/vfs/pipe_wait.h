/* Copyright (C) 2026 NOS contributors. Private to server.c. */
#ifndef NOS_VFS_PIPE_WAIT_H
#define NOS_VFS_PIPE_WAIT_H
struct pending_io { struct nos_message message; unsigned description, used; };
static struct pending_io pending_io[NOS_TASKS];
static unsigned defer_reply;
static int pipe_defer(struct nos_message *message, unsigned description)
{
    for (unsigned i = 0; i < NOS_TASKS; i++) if (!pending_io[i].used) {
        pending_io[i].message = *message; pending_io[i].description = description;
        pending_io[i].used = 1; opened[description].refs++; defer_reply = 1; return 0;
    }
    return -NOS_ENOSPC;
}
static void pipe_cancel(intptr_t sender, uint64_t token, int by_slot)
{
    if (sender < 0) return;
    for (unsigned i = 0; i < NOS_TASKS; i++) {
        struct pending_io *wait = &pending_io[i]; if (!wait->used) continue;
        int matches = by_slot ?
            (uintptr_t)wait->message.sender % NOS_TASKS == (uintptr_t)sender % NOS_TASKS && wait->message.token <= token :
            wait->message.sender == sender;
        if (matches) { wait->used = 0; drop_description(wait->description); }
    }
}
#ifdef NOS_LINUX_COMPAT
/* Authoritative group death retires every syscall pin, even when the final
 * task-slot exit/cancel notice was overwritten by a later generation. */
static void pending_owner_exit(uintptr_t owner)
{
    for (unsigned i = 0; i < NOS_TASKS; i++) {
        struct pending_io *wait = &pending_io[i];
        if (wait->used && wait->message.process == owner) {
            wait->used = 0; drop_description(wait->description);
        }
    }
}
#endif
static int pipe_can_complete(const struct pending_io *wait)
{
    if (wait->used != 1) return 0;
    const struct description *description = &opened[wait->description];
    const struct pipe_buffer *pipe = &pipes[description->pipe];
    if (wait->message.op == NOS_VFS_READ) return !wait->message.len || pipe->size || !pipe->writers;
    return !wait->message.len || !pipe->readers || wait->message.len <= PIPE_BYTES - pipe->size;
}
static void pipe_complete(void)
{
    for (;;) {
        int next = -1;
        for (unsigned i = 0; i < NOS_TASKS; i++) if (pending_io[i].used && pipe_can_complete(&pending_io[i]) &&
            (next < 0 || pending_io[i].message.token < pending_io[next].message.token)) next = (int)i;
        if (next < 0) return;
        struct pending_io *wait = &pending_io[next]; struct nos_message message = wait->message;
        struct description *description = &opened[wait->description]; struct pipe_buffer *pipe = &pipes[description->pipe];
        size_t count = message.len;
        if (message.op == NOS_VFS_READ) {
            if (count > pipe->size) count = pipe->size;
            for (size_t i = 0; i < count; i++) message.data[i] = pipe->bytes[(pipe->head + i) % PIPE_BYTES];
            message.value = (intptr_t)count; message.len = count;
        } else message.value = count && !pipe->readers ? -32 : (intptr_t)count;
        /* Commit only after token-checked copy/reply succeeds. Cancelled calls
         * must neither consume existing bytes nor inject new ones. */
        intptr_t replied = nos_syscall(NOS_SYS_REPLY, (uintptr_t)message.sender, (uintptr_t)&message, 0);
        if (!replied && message.value > 0) {
            if (message.op == NOS_VFS_READ) { pipe->head = (pipe->head + count) % PIPE_BYTES; pipe->size -= count; }
            else { for (size_t i = 0; i < count; i++) pipe->bytes[(pipe->head + pipe->size + i) % PIPE_BYTES] = message.data[i]; pipe->size += count; }
        }
        wait->used = 0; drop_description(wait->description);
    }
}
#endif
