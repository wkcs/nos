/* Copyright (C) 2026 NOS contributors. Private user-space POSIX process record locks. */
#ifndef NOS_VFS_RECORD_LOCK_H
#define NOS_VFS_RECORD_LOCK_H
#ifdef NOS_LINUX_COMPAT
#define RECORD_LOCK_LIMIT 64u
#define RECORD_WORK_LIMIT (RECORD_LOCK_LIMIT + 2u)
#define RECORD_WAIT_KIND 2u
#define RECORD_TO_EOF UINT64_MAX
struct record_range {
    uint64_t inode, start, end;
    uint32_t owner;
    uint16_t type, reserved;
};
struct record_wait { struct record_range range; unsigned fd, command; };
static struct record_range record_locks[RECORD_LOCK_LIMIT];
static struct record_wait record_waits[NOS_TASKS];
static unsigned record_lock_count;
_Static_assert(sizeof(struct record_range) == 32, "compact record lock");
_Static_assert(sizeof(struct nos_record_lock) == 32, "fixed record-lock wire");
static int record_overlap(const struct record_range *a, const struct record_range *b)
{ return a->inode == b->inode && a->start <= b->end && b->start <= a->end; }
static int record_conflict(const struct record_range *a, const struct record_range *b)
{
    return a->owner != b->owner && record_overlap(a, b) &&
        (a->type == NOS_LOCK_WRITE || b->type == NOS_LOCK_WRITE);
}
static int record_first_conflict(const struct record_range *request)
{
    int first = -1;
    for (unsigned i = 0; i < record_lock_count; i++) if (record_conflict(request, &record_locks[i]) &&
        (first < 0 || record_locks[i].start < record_locks[first].start ||
         (record_locks[i].start == record_locks[first].start && record_locks[i].owner < record_locks[first].owner))) first = (int)i;
    return first;
}
static int record_inode(const struct description *description, uint64_t *inode)
{
    if (description->kind && description->kind != ARCHIVE_FILE) return -NOS_ENOTSUP;
    struct nos_stat64 stat; int rc = full_stat(description, &stat); if (rc) return rc;
    if ((stat.mode & 0170000u) != MODE_FILE) return -NOS_ENOTSUP;
    *inode = stat.ino; return 0;
}
static int record_normalize(const struct nos_record_lock *wire, const struct description *description,
                            uint64_t inode, uint32_t owner, struct record_range *range)
{
    uint64_t base;
    if (wire->whence == 0) base = 0;
    else if (wire->whence == 1) base = description->offset;
    else if (wire->whence == 2) base = description->kind == ARCHIVE_FILE ? description->size : nodes[description->node].size;
    else return -NOS_EINVAL;
    if (base > INT64_MAX) return -NOS_EOVERFLOW;
    uint64_t start;
    if (wire->start < 0) {
        uint64_t magnitude = (uint64_t)(-(wire->start + 1)) + 1;
        if (magnitude > base) return -NOS_EINVAL;
        start = base - magnitude;
    } else {
        if ((uint64_t)wire->start > INT64_MAX - base) return -NOS_EOVERFLOW;
        start = base + (uint64_t)wire->start;
    }
    uint64_t end;
    if (!wire->length) end = RECORD_TO_EOF;
    else if (wire->length > 0) {
        if ((uint64_t)wire->length - 1 > INT64_MAX - start) return -NOS_EOVERFLOW;
        end = start + (uint64_t)wire->length - 1;
    } else {
        uint64_t magnitude = (uint64_t)(-(wire->length + 1)) + 1;
        if (magnitude > start || !start) return -NOS_EINVAL;
        end = start - 1; start -= magnitude;
    }
    *range = (struct record_range){ .inode = inode, .start = start, .end = end, .owner = owner, .type = (uint16_t)wire->type };
    return 0;
}
static int record_push(struct record_range *work, unsigned *count, const struct record_range *range)
{
    if (*count == RECORD_WORK_LIMIT) return -NOS_ENOLCK;
    work[(*count)++] = *range; return 0;
}
static int record_before(const struct record_range *a, const struct record_range *b)
{
    return a->inode < b->inode || (a->inode == b->inode &&
        (a->owner < b->owner || (a->owner == b->owner && a->start < b->start)));
}
/* Build a complete candidate table. No successful split/merge becomes visible
 * until its caller has successfully received a token-checked reply. */
static int record_transform(const struct record_range *request, struct record_range *work, unsigned *count)
{
    *count = 0;
    for (unsigned i = 0; i < record_lock_count; i++) {
        struct record_range old = record_locks[i]; int rc;
        if (old.owner != request->owner || !record_overlap(&old, request)) {
            rc = record_push(work, count, &old); if (rc) return rc; continue;
        }
        if (old.start < request->start) {
            struct record_range left = old; left.end = request->start - 1;
            rc = record_push(work, count, &left); if (rc) return rc;
        }
        if (request->end != RECORD_TO_EOF && old.end > request->end && request->end < INT64_MAX) {
            struct record_range right = old; right.start = request->end + 1;
            rc = record_push(work, count, &right); if (rc) return rc;
        }
    }
    if (request->type != NOS_LOCK_UNLOCK) { int rc = record_push(work, count, request); if (rc) return rc; }
    for (unsigned i = 1; i < *count; i++) {
        struct record_range value = work[i]; unsigned at = i;
        while (at && record_before(&value, &work[at - 1])) { work[at] = work[at - 1]; at--; }
        work[at] = value;
    }
    unsigned compact = 0;
    for (unsigned i = 0; i < *count; i++) {
        struct record_range *previous = compact ? &work[compact - 1] : 0;
        struct record_range next = work[i];
        if (previous && previous->inode == next.inode && previous->owner == next.owner && previous->type == next.type &&
            (previous->end == RECORD_TO_EOF || next.start <= previous->end ||
             (previous->end < INT64_MAX && next.start == previous->end + 1))) {
            uint64_t end = previous->end > next.end ? previous->end : next.end;
            /* Keep finite wire lengths representable even at off_t limits. */
            if (end == RECORD_TO_EOF || end - previous->start < INT64_MAX) { previous->end = end; continue; }
        }
        work[compact++] = next;
    }
    if (compact > RECORD_LOCK_LIMIT) return -NOS_ENOLCK;
    *count = compact; return 0;
}
/* The serial service loop traverses process owners, not threads. Every edge
 * targets a held lock, so there are at most RECORD_LOCK_LIMIT distinct owners.
 * Keep this bounded scratch off the stack rather than recursing per task. */
static uint32_t record_owner_queue[RECORD_LOCK_LIMIT];
static int record_owner_add(uint32_t owner, uint32_t target, unsigned *count)
{
    if (owner == target) return -NOS_EDEADLK;
    for (unsigned i = 0; i < *count; i++) if (record_owner_queue[i] == owner) return 0;
    if (*count == RECORD_LOCK_LIMIT) return -NOS_ENOLCK;
    record_owner_queue[(*count)++] = owner; return 0;
}
static int record_deadlock(const struct record_range *request)
{
    unsigned count = 0;
    for (unsigned i = 0; i < record_lock_count; i++) if (record_conflict(request, &record_locks[i])) {
        int rc = record_owner_add(record_locks[i].owner, request->owner, &count);
        if (rc) return rc;
    }
    for (unsigned next = 0; next < count; next++)
        for (unsigned i = 0; i < NOS_TASKS; i++) if (pending_io[i].used == RECORD_WAIT_KIND &&
            record_waits[i].range.owner == record_owner_queue[next])
            for (unsigned j = 0; j < record_lock_count; j++) if (record_conflict(&record_waits[i].range, &record_locks[j])) {
                int rc = record_owner_add(record_locks[j].owner, request->owner, &count);
                if (rc) return rc;
            }
    return 0;
}
static int record_fd_current(const struct pending_io *pending, const struct record_wait *wait)
{
    for (unsigned i = 0; i < FD_TABLE_COUNT; i++) if (fds[i].pid == (intptr_t)wait->range.owner)
        return wait->fd < FDS && fds[i].fd[wait->fd] == pending->description + 1;
    return 0;
}
static void record_lock_service(struct nos_message *message, struct fd_table *table)
{
    if (message->arg < 0 || (uintptr_t)message->arg >= FDS || !table->fd[message->arg]) { message->value = -NOS_EBADF; return; }
    if ((uintptr_t)message->value > NOS_LOCK_SET_WAIT || message->len != sizeof(struct nos_record_lock)) {
        message->value = -NOS_EINVAL; return;
    }
    struct nos_record_lock wire; u_memcpy(&wire, message->data, sizeof(wire));
    if (wire.reserved || wire.type < NOS_LOCK_READ || wire.type > NOS_LOCK_UNLOCK ||
        (message->value == NOS_LOCK_GET && wire.type == NOS_LOCK_UNLOCK)) { message->value = -NOS_EINVAL; return; }
    unsigned command = (unsigned)message->value, fd = (unsigned)message->arg, description = table->fd[fd] - 1;
    struct description *opened_file = &opened[description]; uint64_t inode;
    int rc = record_inode(opened_file, &inode); if (rc) { message->value = rc; return; }
    if (command != NOS_LOCK_GET && ((wire.type == NOS_LOCK_READ && (opened_file->flags & 3) == NOS_O_WRONLY) ||
        (wire.type == NOS_LOCK_WRITE && !(opened_file->flags & 3)))) { message->value = -NOS_EBADF; return; }
    if (message->process > INT32_MAX || !message->process) { message->value = -NOS_EOVERFLOW; return; }
    struct record_range range;
    rc = record_normalize(&wire, opened_file, inode, (uint32_t)message->process, &range); if (rc) { message->value = rc; return; }
    int conflicting = wire.type == NOS_LOCK_UNLOCK ? -1 : record_first_conflict(&range);
    if (command == NOS_LOCK_GET) {
        if (conflicting < 0) wire.type = NOS_LOCK_UNLOCK;
        else { const struct record_range *held = &record_locks[conflicting];
            wire = (struct nos_record_lock){ .start = (int64_t)held->start,
                .length = held->end == RECORD_TO_EOF ? 0 : (int64_t)(held->end - held->start + 1),
                .pid = (int32_t)held->owner, .type = (int16_t)held->type, .whence = 0 };
        }
        u_memcpy(message->data, &wire, sizeof(wire)); message->value = 0; return;
    }
    if (conflicting >= 0 && command == NOS_LOCK_SET) { message->value = -NOS_EAGAIN; return; }
    if (conflicting >= 0) {
        int rc = record_deadlock(&range);
        if (rc) { message->value = rc; return; }
    }
    for (unsigned i = 0; i < NOS_TASKS; i++) if (!pending_io[i].used) {
        pending_io[i].message = *message; pending_io[i].description = description; pending_io[i].used = RECORD_WAIT_KIND;
        record_waits[i] = (struct record_wait){ .range = range, .fd = fd, .command = command };
        opened[description].refs++; defer_reply = 1; return;
    }
    message->value = -NOS_ENOLCK;
}
static void record_lock_complete(void)
{
    for (;;) {
        int next = -1;
        for (unsigned i = 0; i < NOS_TASKS; i++) if (pending_io[i].used == RECORD_WAIT_KIND &&
            (!record_fd_current(&pending_io[i], &record_waits[i]) || record_waits[i].range.type == NOS_LOCK_UNLOCK ||
             record_first_conflict(&record_waits[i].range) < 0) &&
            (next < 0 || pending_io[i].message.token < pending_io[next].message.token)) next = (int)i;
        if (next < 0) return;
        struct pending_io *pending = &pending_io[next]; struct nos_message reply = pending->message;
        struct record_range work[RECORD_WORK_LIMIT]; unsigned count = 0;
        int rc = record_fd_current(pending, &record_waits[next]) ? record_transform(&record_waits[next].range, work, &count) : -NOS_EBADF;
        reply.value = rc;
        intptr_t delivered = nos_syscall(NOS_SYS_REPLY, (uintptr_t)reply.sender, (uintptr_t)&reply, 0);
        if (!delivered && !rc) { u_memcpy(record_locks, work, count * sizeof(work[0])); record_lock_count = count; }
        pending->used = 0; drop_description(pending->description);
    }
}
static void record_lock_close(intptr_t owner, const struct description *description)
{
    uint64_t inode; if (owner < 0 || record_inode(description, &inode)) return;
    unsigned count = 0;
    for (unsigned i = 0; i < record_lock_count; i++)
        if (record_locks[i].owner != (uint32_t)owner || record_locks[i].inode != inode) record_locks[count++] = record_locks[i];
    record_lock_count = count;
}
static void record_lock_owner_exit(intptr_t owner)
{
    unsigned count = 0;
    for (unsigned i = 0; i < record_lock_count; i++)
        if (record_locks[i].owner != (uint32_t)owner) record_locks[count++] = record_locks[i];
    record_lock_count = count;
    pending_owner_exit((uintptr_t)owner);
}
#else
static void record_lock_service(struct nos_message *message, struct fd_table *table)
{ (void)table; message->value = -NOS_ENOTSUP; }
static void record_lock_complete(void) { }
static void record_lock_close(intptr_t owner, const struct description *description) { (void)owner; (void)description; }
#endif
/* Exit notifications are coalesced by task slot. A full-generation query is
 * authoritative; bit 2 retains FD state still referenced by queued forks. */
#ifdef NOS_LINUX_COMPAT
/* The server is single threaded. Query every bounded batch before releasing
 * any owner: a later query failure must preserve all FD, cwd, lock and wait
 * state. Full generation IDs are retained across the entire collection. */
static struct nos_group_state collected_groups[FD_TABLE_COUNT];
_Static_assert(NOS_GROUP_QUERY_MAX > 0, "nonempty GC query batches");
#endif
static void collect_process_owners(void)
{
#ifdef NOS_LINUX_COMPAT
    unsigned count = 0;
    for (unsigned i = 0; i < FD_TABLE_COUNT; i++) if (fds[i].pid >= 0)
        collected_groups[count++] = (struct nos_group_state){ .group = (uint32_t)fds[i].pid, .state = -NOS_ENOTSUP };
    for (unsigned first = 0; first < count; first += NOS_GROUP_QUERY_MAX) {
        unsigned batch = count - first;
        if (batch > NOS_GROUP_QUERY_MAX) batch = NOS_GROUP_QUERY_MAX;
        if (nos_syscall(NOS_SYS_GROUP_ALIVE, (uintptr_t)&collected_groups[first],
                        batch, NOS_GROUP_QUERY_BATCH)) return;
    }
    /* The service cannot mutate fds between batches. Validate the complete
     * returned snapshot before committing even the first owner release. */
    unsigned entry = 0;
    for (unsigned i = 0; i < FD_TABLE_COUNT; i++) if (fds[i].pid >= 0) {
        const struct nos_group_state *group = &collected_groups[entry++];
        if (group->group != (uint32_t)fds[i].pid || group->state < 0 ||
            ((unsigned)group->state & ~(NOS_GROUP_LIVE_TASKS | NOS_GROUP_FORK_REFERENCES))) return;
    }
    entry = 0;
    for (unsigned i = 0; i < FD_TABLE_COUNT; i++) if (fds[i].pid >= 0) {
        const struct nos_group_state *group = &collected_groups[entry++];
        intptr_t state = group->state;
        if (group->group != (uint32_t)fds[i].pid || state < 0 || (state & NOS_GROUP_LIVE_TASKS)) continue;
        record_lock_owner_exit(fds[i].pid);
        if (!state) release_table(&fds[i]);
    }
#endif
}
static void service_complete(void)
{ collect_process_owners(); pipe_complete(); record_lock_complete(); }
#endif
