/* Copyright (C) 2026 NOS contributors */
#include "thread.h"
#include "user.h"
#ifdef NOS_NEWLIB
#include <reent.h>
#include <string.h>
#endif

static void thread_start(uintptr_t argument)
{
    struct nos_thread *thread = (void *)argument;
#ifdef NOS_NEWLIB
    struct _reent local_reent;
    _REENT_INIT_PTR(&local_reent);
#ifdef NOS_NEWLIB_DYNAMIC
    extern int nos_newlib_bind_reent(struct _reent *);
    if (nos_newlib_bind_reent(&local_reent))
#else
    if (nos_syscall(NOS_SYS_TLS_BIND, (uintptr_t)&_impure_ptr, (uintptr_t)&local_reent, 0))
#endif
        nos_process_exit(NOS_EFAULT);
#endif
    void *value = thread->function(thread->argument);
    __atomic_store_n(&thread->result, value, __ATOMIC_RELEASE);
    nos_process_exit(0);
}
int nos_thread_create(struct nos_thread *thread, void *(*function)(void *), void *argument)
{ return nos_thread_create_with_stack(thread, function, argument, NULL, 8192); }
int nos_thread_create_with_stack(struct nos_thread *thread, void *(*function)(void *),
    void *argument, void *stack, size_t size)
{
    int rc = nos_user_check(thread, sizeof(*thread), NOS_PROT_WRITE);
    if (rc) return rc;
    if (!function || size < 4096 || size > 65536 || size % 16) return -NOS_EINVAL;
    intptr_t memory = stack ? (intptr_t)(uintptr_t)stack :
        nos_syscall(NOS_SYS_MMAP, NOS_PROT_READ | NOS_PROT_WRITE, size, 0);
    if (nos_pointer_is_error(memory)) return (int)memory;
    *thread = (struct nos_thread){ .stack = (void *)(uintptr_t)memory, .stack_size = size,
        .function = function, .argument = argument, .owns_stack = !stack };
    struct nos_thread_args args = { .entry = (uintptr_t)thread_start, .arg = (uintptr_t)thread,
        .stack = (uintptr_t)memory, .stack_size = size, .tls = 0,
        .clear_tid = (uintptr_t)&thread->tid, .parent_tid = (uintptr_t)&thread->tid };
    intptr_t tid = nos_syscall(NOS_SYS_THREAD_CREATE, (uintptr_t)&args, UINTPTR_MAX, 0);
    if (tid < 0) {
        if (!stack) nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)memory, size, 0);
        thread->stack = NULL;
        return (int)tid;
    }
    return 0;
}
int nos_thread_join(struct nos_thread *thread, void **result)
{
    int rc = nos_user_check(thread, sizeof(*thread), NOS_PROT_READ | NOS_PROT_WRITE);
    if (rc) return rc;
    if (result && (rc = nos_user_check(result, sizeof(*result), NOS_PROT_WRITE))) return rc;
    if (!thread->stack) return -NOS_EINVAL;
    int tid;
    while ((tid = __atomic_load_n(&thread->tid, __ATOMIC_ACQUIRE)) != 0) {
        if (tid == nos_syscall(NOS_SYS_GETPID, 0, 0, 0)) return -NOS_EDEADLK;
        intptr_t wait = nos_syscall(NOS_SYS_FUTEX_WAIT, (uintptr_t)&thread->tid, (uintptr_t)tid, 0);
        if (wait < 0 && wait != -NOS_EAGAIN && wait != -NOS_EINTR) return (int)wait;
    }
    void *value = __atomic_load_n(&thread->result, __ATOMIC_ACQUIRE);
    rc = thread->owns_stack ? (int)nos_syscall(NOS_SYS_MUNMAP, (uintptr_t)thread->stack, thread->stack_size, 0) : 0;
    if (rc) return rc;
    thread->stack = NULL;
    if (result) *result = value;
    return 0;
}
int nos_lock_acquire(struct nos_lock *lock)
{
    int rc = nos_user_check(lock, sizeof(*lock), NOS_PROT_READ | NOS_PROT_WRITE);
    if (rc) return rc;
    for (;;) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&lock->word, &expected, 1, 0,
                __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return 0;
        intptr_t wait = nos_syscall(NOS_SYS_FUTEX_WAIT, (uintptr_t)&lock->word, 1, 0);
        if (wait < 0 && wait != -NOS_EAGAIN && wait != -NOS_EINTR) return (int)wait;
    }
}
int nos_lock_release(struct nos_lock *lock)
{
    int rc = nos_user_check(lock, sizeof(*lock), NOS_PROT_WRITE);
    if (rc) return rc;
    if (!__atomic_exchange_n(&lock->word, 0, __ATOMIC_RELEASE)) return -NOS_EPERM;
    nos_syscall(NOS_SYS_FUTEX_WAKE, (uintptr_t)&lock->word, 1, 0);
    return 0;
}
int nos_condition_wait(struct nos_condition *condition, struct nos_lock *lock)
{
    int rc = nos_user_check(condition, sizeof(*condition), NOS_PROT_READ);
    if (rc) return rc;
    int sequence = __atomic_load_n(&condition->sequence, __ATOMIC_ACQUIRE);
    rc = nos_lock_release(lock);
    if (rc) return rc;
    intptr_t wait = nos_syscall(NOS_SYS_FUTEX_WAIT, (uintptr_t)&condition->sequence,
        (uintptr_t)sequence, 0);
    rc = nos_lock_acquire(lock);
    if (rc) return rc;
    return wait < 0 && wait != -NOS_EAGAIN && wait != -NOS_EINTR ? (int)wait : 0;
}
int nos_condition_signal(struct nos_condition *condition, int all)
{
    int rc = nos_user_check(condition, sizeof(*condition), NOS_PROT_READ | NOS_PROT_WRITE);
    if (rc) return rc;
    __atomic_add_fetch(&condition->sequence, 1, __ATOMIC_RELEASE);
    return (int)nos_syscall(NOS_SYS_FUTEX_WAKE, (uintptr_t)&condition->sequence,
        all ? NOS_TASKS : 1, 0);
}
