/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
static struct endpoint { unsigned used, generation, owner; } endpoints[NOS_ENDPOINTS];
static uint64_t request_sequence;
static unsigned exit_ids[NOS_TASKS], exit_groups[NOS_TASKS], exit_alive[NOS_TASKS], exit_pending;
struct cancel_notice { unsigned id, group; uint64_t token; };
static struct cancel_notice canceled[NOS_ENDPOINTS][NOS_TASKS];
static unsigned cancel_pending[NOS_ENDPOINTS];
#define PROCESS_NOTICES NOS_IPC_PROCESS_NOTICE_LIMIT
static struct nos_message process_notices[PROCESS_NOTICES];
static unsigned process_head, process_count;
/* A delivered FORK still needs its parent's FD table until the server has
 * processed it. Its next valid RECV implicitly acknowledges that delivery. */
static unsigned delivered_fork_group, delivered_fork_valid;
static void notify_cancel(struct task *client);
static unsigned endpoint_handle(unsigned i)
{
    return i ? NOS_ENDPOINT_BASE + endpoints[i].generation * NOS_ENDPOINTS + i : 0;
}
static struct endpoint *lookup(uintptr_t handle, unsigned *index)
{
    if (handle >= NOS_ENDPOINT_LIMIT || (handle && handle < NOS_ENDPOINT_BASE)) return NULL;
    unsigned i = handle ? ((unsigned)handle - NOS_ENDPOINT_BASE) % NOS_ENDPOINTS : 0;
    if (!endpoints[i].used || endpoint_handle(i) != handle) return NULL;
    *index = i; return &endpoints[i];
}
void ipc_init(void)
{
    endpoints[0].used = 1; endpoints[0].owner = tasks[0].id;
    cancel_pending[0] = 0; exit_pending = 0; process_head = process_count = 0;
    delivered_fork_valid = 0;
    tasks[0].endpoint_rights[0] = NOS_RIGHT_RECV | NOS_RIGHT_GRANT;
    for (unsigned i = 1; i < NOS_TASKS; i++) tasks[i].endpoint_rights[0] = NOS_RIGHT_CALL;
}
unsigned ipc_group_fork_references(unsigned group)
{
    if (delivered_fork_valid && delivered_fork_group == group) return NOS_GROUP_FORK_REFERENCES;
    for (unsigned n = 0; n < process_count; n++) {
        const struct nos_message *notice = &process_notices[(process_head + n) % PROCESS_NOTICES];
        if (notice->op == NOS_IPC_PROCESS_FORK && notice->process == group)
            return NOS_GROUP_FORK_REFERENCES;
    }
    return 0;
}
int ipc_process_notice(intptr_t operation, unsigned process, unsigned value)
{
    if (operation != NOS_IPC_PROCESS_FORK && operation != NOS_IPC_PROCESS_EXEC) return -NOS_EINVAL;
    if (!endpoints[0].used) return -NOS_ESRCH;
    struct task *server = find_task(endpoints[0].owner);
    if (!server || server->state == TASK_DEAD) return -NOS_ESRCH;
    if (process_count == PROCESS_NOTICES) return -NOS_EAGAIN;
    struct nos_message notice; k_memset(&notice, 0, sizeof(notice));
    notice.op = operation; notice.process = process; notice.value = value;
    if (!process_count && server->state == TASK_RECV && server->endpoint_handle == 0 &&
        !task_copy_out(server, server->pending, &notice, sizeof(notice))) {
        if (operation == NOS_IPC_PROCESS_FORK) {
            delivered_fork_group = process; delivered_fork_valid = 1;
        }
        server->state = TASK_READY;
    } else {
        process_notices[(process_head + process_count) % PROCESS_NOTICES] = notice; process_count++;
    }
    reschedule = 1; return 0;
}
void ipc_task_inherit(struct task *child, const struct task *parent)
{
    for (unsigned i = 0; i < NOS_ENDPOINTS; i++) {
        child->endpoint_rights[i] = parent->endpoint_rights[i] & NOS_RIGHT_CALL;
        if (i == 0 && child->caps & NOS_CAP_IPC) child->endpoint_rights[i] |= NOS_RIGHT_CALL;
    }
}
unsigned endpoint_count(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < NOS_ENDPOINTS; i++) count += endpoints[i].used;
    return count;
}
intptr_t endpoint_create(void)
{
    if (!(current_task->caps & NOS_CAP_IPC)) return -NOS_EACCES;
    for (unsigned i = 1; i < NOS_ENDPOINTS; i++) {
        if (endpoints[i].used || endpoints[i].generation > (NOS_ENDPOINT_LIMIT - 1u - NOS_ENDPOINT_BASE - i) / NOS_ENDPOINTS) continue;
        endpoints[i].used = 1; endpoints[i].owner = current_task->id; cancel_pending[i] = 0;
        current_task->endpoint_rights[i] = NOS_RIGHT_CALL | NOS_RIGHT_RECV | NOS_RIGHT_GRANT;
        return endpoint_handle(i);
    }
    return -NOS_ENOSPC;
}
intptr_t endpoint_close(uintptr_t handle)
{
    unsigned i; struct endpoint *ep = lookup(handle, &i);
    if (!ep) return -NOS_EINVAL;
    if (!(current_task->endpoint_rights[i] & NOS_RIGHT_GRANT) &&
        !(current_task->caps & NOS_CAP_SPAWN)) return -NOS_EACCES;
    ep->used = 0; cancel_pending[i] = 0;
    if (!i) { process_head = process_count = 0; delivered_fork_valid = 0; }
    if (i) ep->generation++;
    for (unsigned n = 0; n < NOS_TASKS; n++) {
        struct task *peer = &tasks[n]; peer->endpoint_rights[i] = 0;
        if ((peer->state == TASK_REPLY || peer->state == TASK_RECV) && peer->endpoint_handle == handle) {
            peer->state = TASK_READY; peer->request_sent = 0;
            FRAME_RESULT(&peer->frame) = (uintptr_t)-NOS_ESRCH;
        }
    }
    mutex_recompute(); reschedule = 1; return 0;
}
intptr_t endpoint_grant(uintptr_t pid, uintptr_t handle, uintptr_t rights, int revoke)
{
    unsigned i; struct endpoint *ep = lookup(handle, &i); struct task *peer = find_task(pid);
    if (!ep || !peer || peer->state == TASK_DEAD) return -NOS_ESRCH;
    if (rights & ~(uintptr_t)(NOS_RIGHT_CALL | NOS_RIGHT_RECV | NOS_RIGHT_GRANT)) return -NOS_EINVAL;
    if (!(current_task->endpoint_rights[i] & NOS_RIGHT_GRANT) &&
        !(current_task->caps & NOS_CAP_SPAWN)) return -NOS_EACCES;
    if ((rights & NOS_RIGHT_RECV) && !(current_task->caps & NOS_CAP_SPAWN) && ep->owner != current_task->id) return -NOS_EACCES;
    if (revoke) {
        peer->endpoint_rights[i] &= ~(unsigned)rights;
        if ((rights & NOS_RIGHT_CALL) && peer->state == TASK_REPLY && peer->endpoint_handle == handle) {
            notify_cancel(peer); peer->state = TASK_READY; FRAME_RESULT(&peer->frame) = (uintptr_t)-NOS_EACCES;
        }
        if ((rights & NOS_RIGHT_RECV) && peer->state == TASK_RECV && peer->endpoint_handle == handle) {
            peer->state = TASK_READY; FRAME_RESULT(&peer->frame) = (uintptr_t)-NOS_EACCES;
        }
        if ((rights & NOS_RIGHT_RECV) && ep->owner == peer->id) {
            for (unsigned n = 0; n < NOS_TASKS; n++) {
                struct task *client = &tasks[n];
                if (client->state == TASK_REPLY && client->endpoint_handle == handle) {
                    notify_cancel(client); client->state = TASK_READY;
                    FRAME_RESULT(&client->frame) = (uintptr_t)-NOS_EACCES;
                }
            }
        }
    } else {
        if (rights & NOS_RIGHT_RECV) {
            struct task *old = find_task(ep->owner);
            if (old && old->state == TASK_RECV && old->endpoint_handle == handle) return -NOS_EBUSY;
            for (unsigned n = 0; n < NOS_TASKS; n++) {
                struct task *client = &tasks[n];
                if (client->state == TASK_REPLY && client->endpoint_handle == handle && client->request_sent)
                    return -NOS_EBUSY;
            }
            if (old) old->endpoint_rights[i] &= ~NOS_RIGHT_RECV;
            ep->owner = peer->id;
        }
        peer->endpoint_rights[i] |= (unsigned)rights;
    }
    mutex_recompute(); reschedule = 1; return 0;
}
/* One outstanding call per task makes a latest-token notice per slot sufficient.
 * A canceled delivered request must release service-side references even when
 * the caller has already reused its task slot or issued another call. */
static void notify_cancel(struct task *client)
{
    if (!client->request_sent) return;
    unsigned i; struct endpoint *ep = lookup(client->endpoint_handle, &i);
    client->request_sent = 0;
    if (!ep) return;
    struct task *server = find_task(ep->owner);
    if (!server || server->state == TASK_DEAD || server->state == TASK_RETIRED) return;
    unsigned slot = client->id % NOS_TASKS;
    struct nos_message notice = { .sender = client->id, .op = NOS_IPC_PEER_CANCEL,
        .process = client->space_owner, .token = client->request.token };
    if ((i || !process_count) && server->state == TASK_RECV && server->endpoint_handle == client->endpoint_handle &&
        !task_copy_out(server, server->pending, &notice, sizeof(notice))) {
        server->state = TASK_READY;
    } else {
        canceled[i][slot] = (struct cancel_notice){ client->id, client->space_owner, client->request.token };
        cancel_pending[i] |= 1u << slot;
    }
}
static struct task *oldest_request(unsigned handle)
{
    struct task *selected = NULL;
    for (unsigned i = 0; i < NOS_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_REPLY && t->endpoint_handle == handle && !t->request_sent &&
            (!selected || t->request_order < selected->request_order)) selected = t;
    }
    return selected;
}
intptr_t ipc_call(uintptr_t handle, uintptr_t request, uintptr_t reply, uint64_t timeout)
{
    unsigned i; struct endpoint *ep = lookup(handle, &i);
    if (!(current_task->caps & NOS_CAP_IPC) || !ep || !(current_task->endpoint_rights[i] & NOS_RIGHT_CALL)) return -NOS_EACCES;
    struct task *server = find_task(ep->owner);
    if (!server || server->state == TASK_DEAD || !(server->endpoint_rights[i] & NOS_RIGHT_RECV)) return -NOS_ESRCH;
    struct task *walk = server;
    for (unsigned hop = 0; walk && hop < NOS_TASKS; hop++) {
        if (walk == current_task) return -NOS_EDEADLK;
        walk = task_dependency(walk);
    }
    if (timeout > 100000) return -NOS_EINVAL;
    if (!user_access(current_task, reply, sizeof(struct nos_message), 1) ||
        user_copy_in(&current_task->request, request, sizeof(struct nos_message))) return -NOS_EFAULT;
    if (current_task->request.len > NOS_MSG_DATA) return -NOS_EINVAL;
#ifdef NOS_LINUX_COMPAT
    if (current_task->request.bulk_in_len > NOS_IPC_BULK_MAX || current_task->request.bulk_out_len > NOS_IPC_BULK_MAX) return -NOS_EINVAL;
    if ((current_task->request.bulk_in_len && !user_access(current_task, current_task->request.bulk_in, current_task->request.bulk_in_len, 0)) ||
        (current_task->request.bulk_out_len && !user_access(current_task, current_task->request.bulk_out, current_task->request.bulk_out_len, 1))) return -NOS_EFAULT;
#endif
    if (current_task->request.op < 0) return -NOS_EPERM;
    if (request_sequence == UINT64_MAX) return -NOS_EOVERFLOW;
    current_task->request.sender = current_task->id;
    current_task->request.process = current_task->space_owner;
    current_task->request_order = ++request_sequence;
    current_task->request.token = request_sequence;
#ifdef NOS_LINUX_COMPAT
    current_task->request_subject.token = request_sequence;
    current_task->request_subject.sender = current_task->id;
    current_task->request_subject.process = current_task->space_owner;
    current_task->request_subject.credentials = *credentials_get(current_task);
#endif
    current_task->request_sent = 0; current_task->pending = reply;
    current_task->endpoint_handle = (unsigned)handle;
    current_task->wake_tick = timeout ? kernel_ticks + timeout : 0;
    current_task->reply_from = server->id; current_task->state = TASK_REPLY;
    if ((i || !process_count) && server->state == TASK_RECV && server->endpoint_handle == handle) {
        if (task_copy_out(server, server->pending, &current_task->request, sizeof(struct nos_message))) {
            current_task->state = TASK_READY; return -NOS_EFAULT;
        }
        current_task->request_sent = 1; server->state = TASK_READY;
    }
    mutex_recompute(); reschedule = 1; return 0;
}
intptr_t ipc_recv(uintptr_t handle, uintptr_t buffer)
{
    unsigned i; struct endpoint *ep = lookup(handle, &i);
    if (!ep || ep->owner != current_task->id || !(current_task->endpoint_rights[i] & NOS_RIGHT_RECV)) return -NOS_EACCES;
    if (!user_access(current_task, buffer, sizeof(struct nos_message), 1)) return -NOS_EFAULT;
    if (!i) delivered_fork_valid = 0;
    if (!i && process_count) {
        int rc = user_copy_out(buffer, &process_notices[process_head], sizeof(struct nos_message));
        if (!rc) {
            if (process_notices[process_head].op == NOS_IPC_PROCESS_FORK) {
                delivered_fork_group = (unsigned)process_notices[process_head].process;
                delivered_fork_valid = 1;
            }
            process_head = (process_head + 1) % PROCESS_NOTICES; process_count--;
        }
        return rc;
    }
    if (cancel_pending[i]) {
        unsigned slot = 0;
        while (!(cancel_pending[i] & (1u << slot))) slot++;
        struct cancel_notice *old = &canceled[i][slot];
        struct nos_message notice = { .sender = old->id, .op = NOS_IPC_PEER_CANCEL,
            .token = old->token, .process = old->group };
        cancel_pending[i] &= ~(1u << slot);
        return user_copy_out(buffer, &notice, sizeof(notice));
    }
    if (!i && exit_pending) {
        unsigned slot = 0;
        while (!(exit_pending & (1u << slot))) slot++;
        struct nos_message notice = { .sender = exit_ids[slot], .op = NOS_IPC_PEER_EXIT,
            .process = exit_groups[slot], .value = exit_alive[slot] };
        exit_pending &= ~(1u << slot); return user_copy_out(buffer, &notice, sizeof(notice));
    }
    struct task *client = oldest_request((unsigned)handle);
    if (client) { client->request_sent = 1; return user_copy_out(buffer, &client->request, sizeof(struct nos_message)); }
    current_task->pending = buffer; current_task->endpoint_handle = (unsigned)handle;
    current_task->state = TASK_RECV; reschedule = 1; return 0;
}
intptr_t ipc_reply(uintptr_t pid, uintptr_t buffer)
{
    struct task *client = find_task(pid); struct nos_message reply;
    if (!client || client->state != TASK_REPLY || !client->request_sent || client->reply_from != current_task->id) return -NOS_EPERM;
    if (user_copy_in(&reply, buffer, sizeof(reply))) return -NOS_EFAULT;
    if (reply.len > NOS_MSG_DATA || reply.token != client->request.token) return -NOS_EINVAL;
    reply.sender = current_task->id; reply.process = current_task->space_owner;
    int rc = task_copy_out(client, client->pending, &reply, sizeof(reply));
    FRAME_RESULT(&client->frame) = (uintptr_t)rc; client->state = TASK_READY;
    client->request_sent = 0; mutex_recompute(); reschedule = 1; return rc;
}
intptr_t ipc_xfer(uintptr_t pointer)
{
#ifdef NOS_LINUX_COMPAT
    struct nos_ipc_xfer args;
    if (user_copy_in(&args, pointer, sizeof(args))) return -NOS_EFAULT;
    struct task *client = find_task((uintptr_t)args.sender);
    if (!client || client->state != TASK_REPLY || !client->request_sent || client->reply_from != current_task->id ||
        args.token != client->request.token) return -NOS_EPERM;
    unsigned index; struct endpoint *endpoint = lookup(client->endpoint_handle, &index);
    if (!endpoint || endpoint->owner != current_task->id || !(current_task->endpoint_rights[index] & NOS_RIGHT_RECV)) return -NOS_EPERM;
    if (args.direction > NOS_IPC_STORE || args.count > NOS_IPC_BULK_MAX) return -NOS_EINVAL;
    uintptr_t base = args.direction == NOS_IPC_FETCH ? client->request.bulk_in : client->request.bulk_out;
    size_t length = args.direction == NOS_IPC_FETCH ? client->request.bulk_in_len : client->request.bulk_out_len;
    if (args.offset > length || args.count > length - args.offset || base > UINTPTR_MAX - args.offset) return -NOS_EINVAL;
    uintptr_t remote = base + args.offset;
    if (!user_access(client, remote, args.count, args.direction == NOS_IPC_STORE) ||
        !user_access(current_task, args.buffer, args.count, args.direction == NOS_IPC_FETCH)) return -NOS_EFAULT;
    unsigned char chunk[128]; size_t copied = 0;
    while (copied < args.count) {
        size_t count = args.count - copied; if (count > sizeof(chunk)) count = sizeof(chunk);
        int rc;
        if (args.direction == NOS_IPC_FETCH) {
            rc = task_copy_in(client, chunk, remote + copied, count);
            if (!rc) rc = user_copy_out(args.buffer + copied, chunk, count);
        } else {
            rc = user_copy_in(chunk, args.buffer + copied, count);
            if (!rc) rc = task_copy_out(client, remote + copied, chunk, count);
        }
        if (rc) return rc;
        copied += count;
    }
    return 0;
#else
    (void)pointer; return -NOS_ENOTSUP;
#endif
}
intptr_t ipc_subject(uintptr_t pointer)
{
#ifdef NOS_LINUX_COMPAT
    struct nos_ipc_subject_query query;
    if (user_copy_in(&query, pointer, sizeof(query))) return -NOS_EFAULT;
    if (query.reserved) return -NOS_EINVAL;
    struct task *client = find_task(query.sender);
    if (!client || client->state != TASK_REPLY || !client->request_sent || client->reply_from != current_task->id ||
        query.token != client->request.token) return -NOS_EPERM;
    unsigned index; struct endpoint *endpoint = lookup(client->endpoint_handle, &index);
    if (!endpoint || endpoint->owner != current_task->id || !(current_task->endpoint_rights[index] & NOS_RIGHT_RECV)) return -NOS_EPERM;
    if (query.output > UINTPTR_MAX || !user_access(current_task, (uintptr_t)query.output, sizeof(struct nos_ipc_subject), 1))
        return -NOS_EFAULT;
    return user_copy_out((uintptr_t)query.output, &client->request_subject, sizeof(client->request_subject));
#else
    (void)pointer; return -NOS_ENOTSUP;
#endif
}
void ipc_interrupt_task(struct task *task, intptr_t error)
{
    if (task->state == TASK_REPLY) notify_cancel(task);
    if (task->state == TASK_REPLY || task->state == TASK_RECV) {
        task->state = TASK_READY; FRAME_RESULT(&task->frame) = (uintptr_t)error;
        mutex_recompute(); reschedule = 1;
    }
}
intptr_t ipc_cancel(uintptr_t pid)
{
    struct task *peer = find_task(pid);
    if (!peer) return -NOS_ESRCH;
    if (peer != current_task && peer->parent != current_task->id && !(current_task->caps & NOS_CAP_SPAWN)) return -NOS_EACCES;
    if (peer->state != TASK_REPLY && peer->state != TASK_RECV) return -NOS_EINVAL;
    if (peer->state == TASK_REPLY) notify_cancel(peer);
    peer->state = TASK_READY; FRAME_RESULT(&peer->frame) = (uintptr_t)-NOS_EINTR;
    mutex_recompute(); reschedule = 1; return 0;
}
void ipc_timeouts(void)
{
    for (unsigned i = 0; i < NOS_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_REPLY && t->wake_tick && t->wake_tick <= kernel_ticks) {
            notify_cancel(t); t->state = TASK_READY; FRAME_RESULT(&t->frame) = (uintptr_t)-NOS_ETIMEDOUT;
        }
    }
    mutex_recompute();
}
void ipc_task_exit(struct task *task)
{
    notify_cancel(task);
    for (unsigned i = 0; i < NOS_ENDPOINTS; i++) {
        task->endpoint_rights[i] = 0;
        if (!endpoints[i].used || endpoints[i].owner != task->id) continue;
        unsigned handle = endpoint_handle(i);
        endpoints[i].used = 0; cancel_pending[i] = 0;
        if (!i) { process_head = process_count = 0; delivered_fork_valid = 0; }
        if (i) endpoints[i].generation++;
        for (unsigned j = 0; j < NOS_TASKS; j++) {
            struct task *client = &tasks[j]; client->endpoint_rights[i] = 0;
            if (client->state == TASK_REPLY && client->endpoint_handle == handle) {
                client->request_sent = 0; client->state = TASK_READY; FRAME_RESULT(&client->frame) = (uintptr_t)-NOS_ESRCH;
            }
        }
    }
    if (task->id % NOS_TASKS != 0 && endpoints[0].used) {
        unsigned alive = 0;
        for (unsigned n = 0; n < NOS_TASKS; n++) {
            struct task *peer = &tasks[n];
            if (peer != task && peer->space_owner == task->space_owner && peer->state != TASK_UNUSED &&
                peer->state != TASK_DEAD && peer->state != TASK_RETIRED) alive++;
        }
        struct task *server = find_task(endpoints[0].owner);
        if (!process_count && server && server->state == TASK_RECV && server->endpoint_handle == 0) {
            struct nos_message notice = { .sender = task->id, .op = NOS_IPC_PEER_EXIT,
                .process = task->space_owner, .value = alive };
            if (!task_copy_out(server, server->pending, &notice, sizeof(notice))) server->state = TASK_READY;
        } else {
            unsigned slot = task->id % NOS_TASKS; exit_ids[slot] = task->id; exit_groups[slot] = task->space_owner;
            exit_alive[slot] = alive; exit_pending |= 1u << slot;
        }
    }
}
