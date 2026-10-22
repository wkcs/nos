/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
static struct endpoint { unsigned used, generation, owner; } endpoints[NOS_ENDPOINTS];
static uint64_t request_sequence;
#define TASK_PENDING_BITS 32u
#define TASK_PENDING_WORDS ((NOS_TASKS + TASK_PENDING_BITS - 1u) / TASK_PENDING_BITS)
struct task_pending { uint32_t words[TASK_PENDING_WORDS]; };
static unsigned exit_ids[NOS_TASKS], exit_groups[NOS_TASKS], exit_alive[NOS_TASKS];
static struct task_pending exit_pending;
struct cancel_notice { unsigned id, group; uint64_t token; };
static struct cancel_notice canceled[NOS_ENDPOINTS][NOS_TASKS];
static struct task_pending cancel_pending[NOS_ENDPOINTS];
#define PROCESS_NOTICES NOS_IPC_PROCESS_NOTICE_LIMIT
static struct nos_message process_notices[PROCESS_NOTICES];
static unsigned process_head, process_count;
/* A delivered FORK still needs its parent's FD table until the server has
 * processed it. Its next valid RECV implicitly acknowledges that delivery. */
static unsigned delivered_fork_group, delivered_fork_valid;
static void pending_reset(struct task_pending *pending)
{ k_memset(pending, 0, sizeof(*pending)); }
static void pending_set(struct task_pending *pending, unsigned slot)
{
    if (slot >= NOS_TASKS) kernel_fail("IPC pending slot outside task pool");
    pending->words[slot / TASK_PENDING_BITS] |= UINT32_C(1) << (slot % TASK_PENDING_BITS);
}
static void pending_clear(struct task_pending *pending, unsigned slot)
{
    if (slot >= NOS_TASKS) kernel_fail("IPC pending slot outside task pool");
    pending->words[slot / TASK_PENDING_BITS] &= ~(UINT32_C(1) << (slot % TASK_PENDING_BITS));
}
static unsigned pending_first(const struct task_pending *pending)
{
    for (unsigned word = 0; word < TASK_PENDING_WORDS; word++) {
        uint32_t bits = pending->words[word];
#if NOS_TASKS % TASK_PENDING_BITS
        if (word == TASK_PENDING_WORDS - 1)
            bits &= UINT32_MAX >> (TASK_PENDING_BITS - NOS_TASKS % TASK_PENDING_BITS);
#endif
        if (!bits) continue;
        unsigned bit = 0;
        while (!(bits & UINT32_C(1))) { bits >>= 1; bit++; }
        return word * TASK_PENDING_BITS + bit;
    }
    return NOS_TASKS;
}
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
    /* Also used for VFS restart: private endpoints and their owners survive,
     * so their delivered-request cancellation notices must survive too. */
    pending_reset(&cancel_pending[0]);
    pending_reset(&exit_pending); process_head = process_count = 0;
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
        endpoints[i].used = 1; endpoints[i].owner = current_task->id; pending_reset(&cancel_pending[i]);
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
    ep->used = 0; pending_reset(&cancel_pending[i]);
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
        pending_set(&cancel_pending[i], slot);
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
static intptr_t call_begin(uintptr_t handle, uintptr_t request, uintptr_t reply, uint64_t timeout,
                           const struct nos_callv_args *vector)
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
    if (vector) {
        current_task->request.bulk_in = vector->input_count ? (uintptr_t)vector->inputs[0].address : 0;
        current_task->request.bulk_in_len = vector->input_count ? (size_t)vector->inputs[0].length : 0;
    }
    if (current_task->request.bulk_in_len > NOS_IPC_BULK_MAX || current_task->request.bulk_out_len > NOS_IPC_BULK_MAX) return -NOS_EINVAL;
    if ((current_task->request.bulk_in_len && !user_access(current_task, current_task->request.bulk_in, current_task->request.bulk_in_len, 0)) ||
        (current_task->request.bulk_out_len && !user_access(current_task, current_task->request.bulk_out, current_task->request.bulk_out_len, 1))) return -NOS_EFAULT;
    if (vector && vector->input_count == 2 && vector->inputs[1].length &&
        !user_access(current_task, (uintptr_t)vector->inputs[1].address, (size_t)vector->inputs[1].length, 0)) return -NOS_EFAULT;
    current_task->request_input_count = vector ? vector->input_count : 1;
    current_task->request_second_input = vector && vector->input_count == 2 ?
        vector->inputs[1] : (struct nos_ipc_span){0, 0};
#else
    (void)vector;
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
intptr_t ipc_call(uintptr_t handle, uintptr_t request, uintptr_t reply, uint64_t timeout)
{ return call_begin(handle, request, reply, timeout, NULL); }
intptr_t ipc_vector(uintptr_t action, uintptr_t pointer, uintptr_t reserved)
{
#ifdef NOS_LINUX_COMPAT
    if (reserved) return -NOS_EINVAL;
    if (action == NOS_IPC_VECTOR_QUERY) return pointer ? -NOS_EINVAL : 2;
    if (action != NOS_IPC_VECTOR_CALL) return -NOS_EINVAL;
    struct nos_callv_args args;
    if (user_copy_in(&args, pointer, sizeof(args))) return -NOS_EFAULT;
    if (args.reserved || args.input_count > 2 || args.endpoint > UINTPTR_MAX) return -NOS_EINVAL;
    if (args.request > UINTPTR_MAX || args.reply > UINTPTR_MAX) return -NOS_EFAULT;
    for (unsigned i = 0; i < 2; i++) {
        if (i >= args.input_count) {
            if (args.inputs[i].address || args.inputs[i].length) return -NOS_EINVAL;
        } else {
            if (args.inputs[i].length > NOS_IPC_BULK_MAX) return -NOS_EINVAL;
            if (args.inputs[i].address > UINTPTR_MAX) return -NOS_EFAULT;
        }
    }
    /* Descriptor copies are immutable before publishing the request. The
     * normal CALL path preflights all READ inputs and the existing WRITE span. */
    return call_begin((uintptr_t)args.endpoint, (uintptr_t)args.request, (uintptr_t)args.reply,
                      args.timeout_ticks, &args);
#else
    (void)action; (void)pointer; (void)reserved; return -NOS_ENOTSUP;
#endif
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
    unsigned slot = pending_first(&cancel_pending[i]);
    if (slot < NOS_TASKS) {
        struct cancel_notice *old = &canceled[i][slot];
        struct nos_message notice = { .sender = old->id, .op = NOS_IPC_PEER_CANCEL,
            .token = old->token, .process = old->group };
        pending_clear(&cancel_pending[i], slot);
        return user_copy_out(buffer, &notice, sizeof(notice));
    }
    slot = pending_first(&exit_pending);
    if (!i && slot < NOS_TASKS) {
        struct nos_message notice = { .sender = exit_ids[slot], .op = NOS_IPC_PEER_EXIT,
            .process = exit_groups[slot], .value = exit_alive[slot] };
        pending_clear(&exit_pending, slot); return user_copy_out(buffer, &notice, sizeof(notice));
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
    if (args.direction > NOS_IPC_FETCH_SECOND || args.count > NOS_IPC_BULK_MAX ||
        (args.direction == NOS_IPC_FETCH && !client->request_input_count) ||
        (args.direction == NOS_IPC_FETCH_SECOND && client->request_input_count < 2)) return -NOS_EINVAL;
    unsigned fetch = args.direction != NOS_IPC_STORE;
    uintptr_t base = args.direction == NOS_IPC_FETCH_SECOND ? (uintptr_t)client->request_second_input.address :
        fetch ? client->request.bulk_in : client->request.bulk_out;
    size_t length = args.direction == NOS_IPC_FETCH_SECOND ? (size_t)client->request_second_input.length :
        fetch ? client->request.bulk_in_len : client->request.bulk_out_len;
    if (args.offset > length || args.count > length - args.offset || base > UINTPTR_MAX - args.offset) return -NOS_EINVAL;
    uintptr_t remote = base + args.offset;
    if (!user_access(client, remote, args.count, !fetch) ||
        !user_access(current_task, args.buffer, args.count, fetch)) return -NOS_EFAULT;
    unsigned char chunk[128]; size_t copied = 0;
    while (copied < args.count) {
        size_t count = args.count - copied; if (count > sizeof(chunk)) count = sizeof(chunk);
        int rc;
        if (fetch) {
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
#ifdef NOS_LINUX_COMPAT
static struct task *subject_client(const struct nos_ipc_subject_query *query)
{
    struct task *client = find_task(query->sender);
    if (!client || client->state != TASK_REPLY || !client->request_sent || client->reply_from != current_task->id ||
        query->token != client->request.token) return NULL;
    unsigned index; struct endpoint *endpoint = lookup(client->endpoint_handle, &index);
    if (!endpoint || endpoint->owner != current_task->id || !(current_task->endpoint_rights[index] & NOS_RIGHT_RECV)) return NULL;
    return client;
}
#endif
intptr_t ipc_subject(uintptr_t pointer)
{
#ifdef NOS_LINUX_COMPAT
    struct nos_ipc_subject_query query;
    if (user_copy_in(&query, pointer, sizeof(query))) return -NOS_EFAULT;
    if (query.reserved) return -NOS_EINVAL;
    struct task *client = subject_client(&query);
    if (!client) return -NOS_EPERM;
    if (query.output > UINTPTR_MAX || !user_access(current_task, (uintptr_t)query.output, sizeof(struct nos_ipc_subject), 1))
        return -NOS_EFAULT;
    return user_copy_out((uintptr_t)query.output, &client->request_subject, sizeof(client->request_subject));
#else
    (void)pointer; return -NOS_ENOTSUP;
#endif
}
intptr_t ipc_limits(uintptr_t pointer)
{
#ifdef NOS_LINUX_COMPAT
    struct nos_ipc_subject_query query;
    if (user_copy_in(&query, pointer, sizeof(query))) return -NOS_EFAULT;
    if (query.reserved) return -NOS_EINVAL;
    struct task *client = subject_client(&query);
    if (!client) return -NOS_EPERM;
    if (query.output > UINTPTR_MAX || !user_access(current_task, (uintptr_t)query.output, sizeof(struct nos_ipc_limits), 1))
        return -NOS_EFAULT;
    uint64_t limits[2]; process_nofile_limits(client, limits);
    /* Unlike immutable CALL credentials, this live query is the service's
     * resource admission point. Later queries observe later limit changes. */
    struct nos_ipc_limits value = { .token = client->request.token,
        .sender = client->id, .process = client->space_owner,
        .nofile_soft = limits[0], .nofile_hard = limits[1] };
    return user_copy_out((uintptr_t)query.output, &value, sizeof(value));
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
        endpoints[i].used = 0; pending_reset(&cancel_pending[i]);
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
            exit_alive[slot] = alive; pending_set(&exit_pending, slot);
        }
    }
}

#if defined(NOS_SELFTEST) && defined(NOS_LINUX_COMPAT)
/* Boot CPU only, before userspace or secondary CPUs run. Synthetic threads
 * borrow the service's address space and credentials without acquiring or
 * releasing either. In particular the real idle task in slot 79 survives
 * byte-for-byte. Whole-task storage also restores priority donation and the
 * rights revoked globally by endpoint close/owner exit. Keep it off the stack. */
static struct {
    struct task tasks[NOS_TASKS];
    struct endpoint endpoints[NOS_ENDPOINTS];
    struct cancel_notice canceled[NOS_ENDPOINTS][NOS_TASKS];
    struct task_pending cancel_pending[NOS_ENDPOINTS], exit_pending;
    unsigned exit_ids[NOS_TASKS], exit_groups[NOS_TASKS], exit_alive[NOS_TASKS];
    struct nos_message process_notices[PROCESS_NOTICES], user_messages[2], private_message;
    unsigned process_head, process_count, delivered_fork_group, delivered_fork_valid;
    uint64_t request_sequence;
} capacity_saved;

static void capacity_client(unsigned slot, unsigned generation)
{
    struct task *client = &tasks[slot];
    *client = capacity_saved.tasks[0];
    client->generation = generation; client->id = generation * NOS_TASKS + slot;
    client->parent = tasks[0].id; client->state = TASK_READY;
    client->caps = NOS_CAP_IPC; client->is_thread = 1;
    client->run_cpu = client->pin_cpu = UINT32_MAX;
    client->base_priority = client->priority = 8;
    client->request_sent = 0; client->wake_tick = 0;
    k_memset(client->endpoint_rights, 0, sizeof(client->endpoint_rights));
    client->endpoint_rights[0] = NOS_RIGHT_CALL;
}

int ipc_capacity_selftest(void)
{
    static const unsigned slots[] = {31, 32, 63, 64, 79};
    enum { COUNT = sizeof(slots) / sizeof(slots[0]) };
    _Static_assert(NOS_TASKS > 79, "IPC capacity regression needs high task slots");
    struct task *previous = current_task, *server = &tasks[0];
    unsigned saved_reschedule = reschedule;
    uintptr_t buffer = USER_DATA + HEAP_OFFSET;
    uintptr_t reply_buffer = buffer + sizeof(struct nos_message);
    struct nos_message message = {0};
    uint64_t tokens[COUNT];
    unsigned ids[COUNT];
    int ok = 0;
    if (task_copy_in(server, capacity_saved.user_messages, buffer,
                     sizeof(capacity_saved.user_messages))) return 0;
    if (task_copy_in(&tasks[1], &capacity_saved.private_message, buffer,
                     sizeof(capacity_saved.private_message))) return 0;
#define SAVE_CAPACITY(field) k_memcpy(&capacity_saved.field, &field, sizeof(field))
#define RESTORE_CAPACITY(field) k_memcpy(&field, &capacity_saved.field, sizeof(field))
    SAVE_CAPACITY(tasks); SAVE_CAPACITY(endpoints); SAVE_CAPACITY(canceled);
    SAVE_CAPACITY(cancel_pending); SAVE_CAPACITY(exit_pending);
    SAVE_CAPACITY(exit_ids); SAVE_CAPACITY(exit_groups); SAVE_CAPACITY(exit_alive);
    SAVE_CAPACITY(process_notices); SAVE_CAPACITY(process_head); SAVE_CAPACITY(process_count);
    SAVE_CAPACITY(delivered_fork_group); SAVE_CAPACITY(delivered_fork_valid);
    SAVE_CAPACITY(request_sequence);
#define REQUIRE_CAPACITY(condition) do { if (!(condition)) { \
    kernel_print("IPC capacity failed: " #condition "\n"); goto restore; \
} } while (0)
    current_task = server; server->state = TASK_READY;
    REQUIRE_CAPACITY(!process_count && pending_first(&exit_pending) == NOS_TASKS);
    for (unsigned i = 0; i < NOS_ENDPOINTS; i++)
        REQUIRE_CAPACITY(pending_first(&cancel_pending[i]) == NOS_TASKS);
    for (unsigned i = 0; i < COUNT; i++) capacity_client(slots[i], 2);

    /* Actual queued CALL delivery and reply checks, including an old full ID
     * after slot reuse and an old token against a valid new-generation ID. */
    for (unsigned i = 0; i < COUNT; i++) {
        struct task *client = &tasks[slots[i]];
        unsigned old_id = client->id;
        message = (struct nos_message){ .op = 7, .len = 1 };
        message.data[0] = (unsigned char)slots[i];
        REQUIRE_CAPACITY(!task_copy_out(server, buffer, &message, sizeof(message)));
        current_task = client;
        REQUIRE_CAPACITY(!ipc_call(0, buffer, reply_buffer, 0));
        REQUIRE_CAPACITY(client->state == TASK_REPLY && !client->request_sent);
        current_task = server;
        REQUIRE_CAPACITY(!ipc_recv(0, buffer));
        REQUIRE_CAPACITY(!task_copy_in(server, &message, buffer, sizeof(message)));
        uint64_t old_token = message.token;
        REQUIRE_CAPACITY(client->request_sent && message.sender == (intptr_t)old_id &&
                         message.process == server->space_owner && old_token &&
                         message.data[0] == (char)slots[i]);
        REQUIRE_CAPACITY(!ipc_cancel(old_id));
        REQUIRE_CAPACITY(client->state == TASK_READY && !client->request_sent &&
                         FRAME_RESULT(&client->frame) == (uintptr_t)-NOS_EINTR);
        REQUIRE_CAPACITY(!ipc_recv(0, buffer));
        REQUIRE_CAPACITY(!task_copy_in(server, &message, buffer, sizeof(message)));
        REQUIRE_CAPACITY(message.op == NOS_IPC_PEER_CANCEL && message.sender == (intptr_t)old_id &&
                         message.process == server->space_owner && message.token == old_token);
        REQUIRE_CAPACITY(ipc_reply(old_id, buffer) == -NOS_EPERM);
        capacity_client(slots[i], 3);
        message = (struct nos_message){ .op = 8 };
        REQUIRE_CAPACITY(!task_copy_out(server, buffer, &message, sizeof(message)));
        current_task = client;
        REQUIRE_CAPACITY(!ipc_call(0, buffer, reply_buffer, 0));
        current_task = server;
        REQUIRE_CAPACITY(!ipc_recv(0, buffer));
        REQUIRE_CAPACITY(!task_copy_in(server, &message, buffer, sizeof(message)));
        uint64_t new_token = message.token;
        REQUIRE_CAPACITY(message.sender == (intptr_t)client->id && new_token > old_token);
        message.token = old_token;
        REQUIRE_CAPACITY(!task_copy_out(server, buffer, &message, sizeof(message)));
        REQUIRE_CAPACITY(ipc_reply(old_id, buffer) == -NOS_EPERM);
        REQUIRE_CAPACITY(ipc_reply(client->id, buffer) == -NOS_EINVAL);
        REQUIRE_CAPACITY(client->state == TASK_REPLY && client->request_sent);
        message.token = new_token; message.len = 1; message.data[0] = (unsigned char)i;
        REQUIRE_CAPACITY(!task_copy_out(server, buffer, &message, sizeof(message)));
        REQUIRE_CAPACITY(!ipc_reply(client->id, buffer));
        REQUIRE_CAPACITY(!task_copy_in(client, &message, reply_buffer, sizeof(message)));
        REQUIRE_CAPACITY(client->state == TASK_READY && !client->request_sent &&
                         message.sender == (intptr_t)server->id && message.process == server->space_owner &&
                         message.token == new_token && message.len == 1 && message.data[0] == (char)i);
    }

    /* Hold every boundary bit at once. Reusing one slot replaces its own
     * notice only; process lifecycle notices retain their delivery priority. */
    for (unsigned i = 0; i < COUNT; i++) {
        struct task *client = &tasks[slots[i]];
        client->state = TASK_REPLY; client->request_sent = 1;
        client->reply_from = server->id; client->endpoint_handle = 0;
        client->request.token = ++request_sequence;
        ids[i] = client->id; tokens[i] = client->request.token;
        REQUIRE_CAPACITY(!ipc_cancel(client->id));
    }
    capacity_client(slots[0], 4);
    tasks[slots[0]].state = TASK_REPLY; tasks[slots[0]].request_sent = 1;
    tasks[slots[0]].endpoint_handle = 0; tasks[slots[0]].reply_from = server->id;
    tasks[slots[0]].request.token = ++request_sequence;
    REQUIRE_CAPACITY(!ipc_cancel(tasks[slots[0]].id));
    REQUIRE_CAPACITY(ipc_reply(ids[0], buffer) == -NOS_EPERM);
    ids[0] = tasks[slots[0]].id; tokens[0] = tasks[slots[0]].request.token;
    REQUIRE_CAPACITY(!ipc_process_notice(NOS_IPC_PROCESS_EXEC, server->space_owner, 17));
    REQUIRE_CAPACITY(!ipc_recv(0, buffer));
    REQUIRE_CAPACITY(!task_copy_in(server, &message, buffer, sizeof(message)));
    REQUIRE_CAPACITY(message.op == NOS_IPC_PROCESS_EXEC && message.value == 17);
    for (unsigned i = 0; i < COUNT; i++) {
        REQUIRE_CAPACITY(!ipc_recv(0, buffer));
        REQUIRE_CAPACITY(!task_copy_in(server, &message, buffer, sizeof(message)));
        REQUIRE_CAPACITY(message.op == NOS_IPC_PEER_CANCEL && message.sender == (intptr_t)ids[i] &&
                         message.process == server->space_owner && message.token == tokens[i]);
    }
    REQUIRE_CAPACITY(pending_first(&cancel_pending[0]) == NOS_TASKS);

    /* VFS reinitialization must leave unrelated private services alone. The
     * private owner borrows init's AS/credentials, not the VFS process group;
     * all five boundary notices remain pending while endpoint 0 is reset. */
    struct task *private_owner = &tasks[30];
    *private_owner = capacity_saved.tasks[1];
    private_owner->id = 2 * NOS_TASKS + 30; private_owner->generation = 2;
    private_owner->state = TASK_READY; private_owner->caps = NOS_CAP_IPC;
    private_owner->run_cpu = private_owner->pin_cpu = UINT32_MAX;
    private_owner->is_thread = 1; private_owner->request_sent = 0;
    k_memset(private_owner->endpoint_rights, 0, sizeof(private_owner->endpoint_rights));
    REQUIRE_CAPACITY(private_owner->space_owner != server->space_owner);
    current_task = private_owner;
    intptr_t private_handle = endpoint_create();
    REQUIRE_CAPACITY(private_handle >= (intptr_t)NOS_ENDPOINT_BASE);
    unsigned private_index = ((unsigned)private_handle - NOS_ENDPOINT_BASE) % NOS_ENDPOINTS;
    for (unsigned i = 0; i < COUNT; i++) {
        struct task *client = &tasks[slots[i]];
        REQUIRE_CAPACITY(!endpoint_grant(client->id, (uintptr_t)private_handle, NOS_RIGHT_CALL, 0));
        message = (struct nos_message){ .op = 9 };
        REQUIRE_CAPACITY(!task_copy_out(client, buffer, &message, sizeof(message)));
        current_task = client;
        REQUIRE_CAPACITY(!ipc_call((uintptr_t)private_handle, buffer, reply_buffer, 0));
        REQUIRE_CAPACITY(client->state == TASK_REPLY && !client->request_sent);
        current_task = private_owner;
        REQUIRE_CAPACITY(!ipc_recv((uintptr_t)private_handle, buffer));
        REQUIRE_CAPACITY(!task_copy_in(private_owner, &message, buffer, sizeof(message)));
        REQUIRE_CAPACITY(message.sender == (intptr_t)ids[i] && client->request_sent);
        tokens[i] = message.token;
    }
    current_task = server;
    for (unsigned i = 0; i < COUNT; i++)
        REQUIRE_CAPACITY(!ipc_cancel(tasks[slots[i]].id));
    /* The same caller can cancel a later VFS call without acknowledging its
     * earlier private-service token; the pending namespaces are independent. */
    struct task *client = &tasks[slots[0]];
    message = (struct nos_message){ .op = 10 };
    REQUIRE_CAPACITY(!task_copy_out(client, buffer, &message, sizeof(message)));
    current_task = client;
    REQUIRE_CAPACITY(!ipc_call(0, buffer, reply_buffer, 0));
    current_task = server;
    REQUIRE_CAPACITY(!ipc_recv(0, buffer));
    REQUIRE_CAPACITY(!ipc_cancel(client->id));
    REQUIRE_CAPACITY(pending_first(&cancel_pending[0]) == slots[0]);
    ipc_init();
    REQUIRE_CAPACITY(pending_first(&cancel_pending[0]) == NOS_TASKS &&
                     pending_first(&cancel_pending[private_index]) == slots[0] &&
                     endpoints[private_index].owner == private_owner->id);
    current_task = private_owner;
    for (unsigned i = 0; i < COUNT; i++) {
        REQUIRE_CAPACITY(!ipc_recv((uintptr_t)private_handle, buffer));
        REQUIRE_CAPACITY(!task_copy_in(private_owner, &message, buffer, sizeof(message)));
        REQUIRE_CAPACITY(message.op == NOS_IPC_PEER_CANCEL && message.sender == (intptr_t)ids[i] &&
                         message.process == server->space_owner && message.token == tokens[i]);
    }
    REQUIRE_CAPACITY(pending_first(&cancel_pending[private_index]) == NOS_TASKS);
    REQUIRE_CAPACITY(!endpoint_close((uintptr_t)private_handle));
    *private_owner = capacity_saved.tasks[30];
    current_task = server;

    /* A close clears every pending word and revokes high-slot rights. Owner
     * death must do the same before releasing blocked high-slot callers. */
    server->caps |= NOS_CAP_IPC;
    intptr_t handle = endpoint_create();
    REQUIRE_CAPACITY(handle >= (intptr_t)NOS_ENDPOINT_BASE);
    unsigned index = ((unsigned)handle - NOS_ENDPOINT_BASE) % NOS_ENDPOINTS;
    for (unsigned i = 0; i < COUNT; i++) {
        struct task *client = &tasks[slots[i]];
        client->endpoint_rights[index] = NOS_RIGHT_CALL;
        client->endpoint_handle = (unsigned)handle;
        client->state = TASK_REPLY; client->request_sent = 1;
        client->request.token = ++request_sequence;
        REQUIRE_CAPACITY(!ipc_cancel(client->id));
    }
    REQUIRE_CAPACITY(!endpoint_close((uintptr_t)handle));
    REQUIRE_CAPACITY(pending_first(&cancel_pending[index]) == NOS_TASKS);
    for (unsigned i = 0; i < COUNT; i++)
        REQUIRE_CAPACITY(!tasks[slots[i]].endpoint_rights[index]);

    current_task = &tasks[slots[0]];
    handle = endpoint_create();
    REQUIRE_CAPACITY(handle >= (intptr_t)NOS_ENDPOINT_BASE);
    index = ((unsigned)handle - NOS_ENDPOINT_BASE) % NOS_ENDPOINTS;
    current_task = server;
    for (unsigned i = 1; i < COUNT; i++) {
        struct task *client = &tasks[slots[i]];
        client->endpoint_rights[index] = NOS_RIGHT_CALL;
        client->endpoint_handle = (unsigned)handle;
        client->state = TASK_REPLY; client->request_sent = 1;
        client->request.token = ++request_sequence;
        REQUIRE_CAPACITY(!ipc_cancel(client->id));
        client->state = TASK_REPLY; client->request_sent = 1;
    }
    tasks[slots[0]].state = TASK_DEAD;
    ipc_task_exit(&tasks[slots[0]]);
    REQUIRE_CAPACITY(!endpoints[index].used && pending_first(&cancel_pending[index]) == NOS_TASKS);
    for (unsigned i = 1; i < COUNT; i++) {
        struct task *client = &tasks[slots[i]];
        REQUIRE_CAPACITY(client->state == TASK_READY && !client->request_sent &&
                         !client->endpoint_rights[index] &&
                         FRAME_RESULT(&client->frame) == (uintptr_t)-NOS_ESRCH);
    }

    /* Include the owner's already-queued exit and retain all later high-slot
     * exits simultaneously. Only the service and five fixtures share group 0. */
    for (unsigned n = 1; n < NOS_TASKS; n++) {
        unsigned fixture = 0;
        for (unsigned i = 0; i < COUNT; i++) fixture |= n == slots[i];
        REQUIRE_CAPACITY(fixture || tasks[n].state == TASK_UNUSED ||
                         tasks[n].space_owner != server->space_owner);
    }
    for (unsigned i = 1; i < COUNT; i++) {
        tasks[slots[i]].state = TASK_DEAD;
        ipc_task_exit(&tasks[slots[i]]);
    }
    for (unsigned i = 0; i < COUNT; i++) {
        REQUIRE_CAPACITY(!ipc_recv(0, buffer));
        REQUIRE_CAPACITY(!task_copy_in(server, &message, buffer, sizeof(message)));
        REQUIRE_CAPACITY(message.op == NOS_IPC_PEER_EXIT && message.sender == (intptr_t)ids[i] &&
                         message.process == server->space_owner && message.value == (intptr_t)(COUNT - i));
    }
    REQUIRE_CAPACITY(pending_first(&exit_pending) == NOS_TASKS);
    ok = 1;
restore:
    RESTORE_CAPACITY(tasks); RESTORE_CAPACITY(endpoints); RESTORE_CAPACITY(canceled);
    RESTORE_CAPACITY(cancel_pending); RESTORE_CAPACITY(exit_pending);
    RESTORE_CAPACITY(exit_ids); RESTORE_CAPACITY(exit_groups); RESTORE_CAPACITY(exit_alive);
    RESTORE_CAPACITY(process_notices); RESTORE_CAPACITY(process_head); RESTORE_CAPACITY(process_count);
    RESTORE_CAPACITY(delivered_fork_group); RESTORE_CAPACITY(delivered_fork_valid);
    RESTORE_CAPACITY(request_sequence);
    if (task_copy_out(server, buffer, capacity_saved.user_messages,
                      sizeof(capacity_saved.user_messages))) ok = 0;
    if (task_copy_out(&tasks[1], buffer, &capacity_saved.private_message,
                      sizeof(capacity_saved.private_message))) ok = 0;
    current_task = previous; reschedule = saved_reschedule;
#undef REQUIRE_CAPACITY
#undef RESTORE_CAPACITY
#undef SAVE_CAPACITY
    return ok;
}
#endif
