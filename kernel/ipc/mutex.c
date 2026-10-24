/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
static struct { unsigned used, generation, owner; } mutexes[NOS_MUTEXES];
static unsigned handle_of(unsigned i) { return mutexes[i].generation * NOS_MUTEXES + i; }
static void release_object(unsigned i)
{
    mutexes[i].owner = UINT32_MAX;
    if (mutexes[i].generation < (NOS_MUTEX_HANDLE_MAX - i) / NOS_MUTEXES) {
        mutexes[i].generation++; mutexes[i].used = 0;
    } else mutexes[i].used = 2; /* Exhausted generations never alias old handles. */
    for (unsigned n = 0; n < NOS_TASKS; n++) tasks[n].mutex_caps &= ~(1u << i);
}
struct task *task_dependency(struct task *task)
{
    if (task->state == TASK_REPLY) return find_task(task->reply_from);
    if (task->state != TASK_LOCK) return NULL;
    unsigned index = task->wait_for % NOS_MUTEXES;
    if (mutexes[index].used != 1 || handle_of(index) != task->wait_for) return NULL;
    return find_task(mutexes[index].owner);
}
void mutex_recompute(void)
{
    for (unsigned i = 0; i < NOS_TASKS; i++) {
        tasks[i].priority = tasks[i].base_priority;
#ifdef NOS_ESP_IDF
        if (tasks[i].compat_priority < tasks[i].priority)
            tasks[i].priority = tasks[i].compat_priority;
#endif
    }
    /* Acyclic donation reaches a fixed point in at most TASKS passes. */
    for (unsigned pass = 0; pass < NOS_TASKS; pass++) {
        unsigned changed = 0;
        for (unsigned i = 0; i < NOS_TASKS; i++) {
            struct task *w = &tasks[i];
            struct task *owner = NULL;
            if (w->state == TASK_REPLY) owner = find_task(w->reply_from);
            else if (w->state == TASK_LOCK) {
                unsigned index = w->wait_for % NOS_MUTEXES;
                if (mutexes[index].used != 1 || handle_of(index) != w->wait_for) continue;
                owner = find_task(mutexes[index].owner);
            } else continue;
            if (owner && owner->priority > w->priority) { owner->priority = w->priority; changed = 1; }
        }
        if (!changed) break;
    }
}
static void unlock_mutex(unsigned i)
{
    struct task *next = NULL;
    for (unsigned n = 0; n < NOS_TASKS; n++) {
        struct task *t = &tasks[n];
        if (t->state == TASK_LOCK && t->wait_for == handle_of(i) &&
            (!next || t->priority < next->priority)) next = t;
    }
    mutexes[i].owner = next ? next->id : UINT32_MAX;
    if (next) { next->state = TASK_READY; FRAME_RESULT(&next->frame) = 0; }
}
void mutex_task_exit(struct task *task)
{
    task->mutex_caps = 0;
    for (unsigned i = 0; i < NOS_MUTEXES; i++) {
        if (mutexes[i].used != 1) continue;
        if (mutexes[i].owner == task->id) unlock_mutex(i);
        unsigned referenced = 0;
        for (unsigned n = 0; n < NOS_TASKS; n++) {
            if (&tasks[n] != task && tasks[n].state != TASK_UNUSED && tasks[n].state != TASK_DEAD &&
                tasks[n].state != TASK_RETIRED && (tasks[n].mutex_caps & (1u << i))) referenced = 1;
        }
        if (!referenced) release_object(i);
    }
    mutex_recompute();
}
unsigned mutex_count(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < NOS_MUTEXES; i++) count += mutexes[i].used == 1;
    return count;
}
intptr_t mutex_operation(unsigned number, uintptr_t wide_handle)
{
    if (number == NOS_SYS_MUTEX_CREATE) {
        for (unsigned i = 0; i < NOS_MUTEXES; i++) {
            if (mutexes[i].used) continue;
            mutexes[i].used = 1; mutexes[i].owner = UINT32_MAX;
            current_task->mutex_caps |= 1u << i;
            for (unsigned n = 0; n < NOS_TASKS; n++) {
                struct task *peer = &tasks[n];
                if (peer->space_id == current_task->space_id && peer->state != TASK_UNUSED &&
                    peer->state != TASK_DEAD && peer->state != TASK_RETIRED) peer->mutex_caps |= 1u << i;
            }
#ifndef NOS_MMU
            for (unsigned n = 0; n < NOS_TASKS; n++) tasks[n].mutex_caps |= 1u << i;
#endif
            return handle_of(i);
        }
        return -NOS_ENOSPC;
    }
    if (wide_handle > NOS_MUTEX_HANDLE_MAX) return -NOS_EINVAL;
    unsigned handle = (unsigned)wide_handle, i = handle % NOS_MUTEXES;
    if (mutexes[i].used != 1 || handle_of(i) != handle) return -NOS_EINVAL;
    if (!(current_task->mutex_caps & (1u << i))) return -NOS_EACCES;
    if (number == NOS_SYS_MUTEX_LOCK) {
        if (mutexes[i].owner == current_task->id) return -NOS_EDEADLK;
        if (mutexes[i].owner == UINT32_MAX) { mutexes[i].owner = current_task->id; return 0; }
        struct task *owner = find_task(mutexes[i].owner);
        if (!owner) return -NOS_ESRCH;
        struct task *walk = owner;
        for (unsigned hop = 0; hop < NOS_TASKS; hop++) {
            if (walk == current_task) return -NOS_EDEADLK;
            walk = task_dependency(walk);
            if (!walk) break;
        }
        current_task->state = TASK_LOCK; current_task->wait_for = handle;
        mutex_recompute(); reschedule = 1; return 0;
    }
    if (number == NOS_SYS_MUTEX_UNLOCK) {
        if (mutexes[i].owner != current_task->id) return -NOS_EPERM;
        unlock_mutex(i); mutex_recompute(); reschedule = 1; return 0;
    }
    if (mutexes[i].owner != UINT32_MAX) return -NOS_EBUSY;
    release_object(i); return 0;
}

intptr_t mutex_grant(uintptr_t pid, uintptr_t handle, uintptr_t rights, int revoke)
{
    struct task *peer = find_task(pid);
    if (!peer || peer->state == TASK_DEAD) return -NOS_ESRCH;
    if (handle > NOS_MUTEX_HANDLE_MAX || rights != NOS_RIGHT_MUTEX) return -NOS_EINVAL;
    unsigned i = (unsigned)handle % NOS_MUTEXES;
    if (mutexes[i].used != 1 || handle_of(i) != handle) return -NOS_EINVAL;
    if (!(current_task->mutex_caps & (1u << i))) return -NOS_EACCES;
    if (!revoke) peer->mutex_caps |= 1u << i;
    else {
        if (mutexes[i].owner == peer->id) return -NOS_EBUSY;
        peer->mutex_caps &= ~(1u << i);
        if (peer->state == TASK_LOCK && peer->wait_for == handle) {
            peer->state = TASK_READY; FRAME_RESULT(&peer->frame) = (uintptr_t)-NOS_EACCES;
        }
        unsigned referenced = 0;
        for (unsigned n = 0; n < NOS_TASKS; n++)
            if (tasks[n].state != TASK_UNUSED && tasks[n].state != TASK_DEAD &&
                (tasks[n].mutex_caps & (1u << i))) referenced = 1;
        if (!referenced) release_object(i);
    }
    mutex_recompute(); reschedule = 1; return 0;
}
