/* Copyright (C) 2026 NOS contributors */
#include "user.h"
#include "thread.h"
#include <pthread.h>
#include <errno.h>
#include <limits.h>

#define POSIX_OBJECTS 16u
#define POSIX_KEYS 8u
#define LAZY_OBJECT UINT32_MAX
struct thread_record {
    unsigned used, generation, joining;
    struct nos_thread native;
    void *(*function)(void *);
    void *argument;
};
struct mutex_record { unsigned used, generation, depth, waiters; int owner, type; };
struct condition_record { unsigned used, generation, waiters; int sequence; clockid_t clock; };
struct key_record { unsigned used, generation; void (*destructor)(void *); };
static struct nos_lock object_lock;
static struct thread_record threads[NOS_TASKS];
static struct mutex_record mutexes[POSIX_OBJECTS];
static struct condition_record conditions[POSIX_OBJECTS];
static struct key_record keys[POSIX_KEYS];
static struct { int tid, cancel_state; void *values[POSIX_KEYS];
    struct _pthread_cleanup_context *cleanup; } locals[NOS_TASKS];
extern int nos_newlib_errno(int code);
static int error(int result) { return result < 0 ? nos_newlib_errno(-result) : result; }
static unsigned tid(void) { return (unsigned)nos_syscall(NOS_SYS_GETPID, 0, 0, 0); }
static unsigned local_index(void)
{
    unsigned identifier = tid(), index = identifier % NOS_TASKS;
    if ((unsigned)locals[index].tid != identifier) {
        u_memset(&locals[index], 0, sizeof(locals[index]));
        locals[index].tid = (int)identifier;
    }
    return index;
}
static void local_cleanup(void)
{
    unsigned index = local_index();
    while (locals[index].cleanup) {
        struct _pthread_cleanup_context *context = locals[index].cleanup;
        locals[index].cleanup = context->_previous;
        context->_routine(context->_arg);
    }
    for (unsigned iteration = 0; iteration < 4; iteration++) {
        unsigned called = 0;
        for (unsigned i = 0; i < POSIX_KEYS; i++) {
            void *value = locals[index].values[i];
            if (value && keys[i].used && keys[i].destructor) {
                locals[index].values[i] = NULL;
                keys[i].destructor(value); called++;
            }
        }
        if (!called) break;
    }
}
static struct thread_record *thread_record(pthread_t handle)
{
    if (!(handle & 0x80000000u)) return NULL;
    unsigned value = handle & 0x7fffffffu;
    struct thread_record *record = &threads[value % NOS_TASKS];
    return record->used && record->generation == value / NOS_TASKS ? record : NULL;
}
static void *thread_entry(void *pointer)
{
    struct thread_record *record = pointer;
    void *value = record->function(record->argument);
    local_cleanup();
    return value;
}
int pthread_create(pthread_t *output, const pthread_attr_t *attributes,
    void *(*function)(void *), void *argument)
{
    if (nos_user_check(output, sizeof(*output), NOS_PROT_WRITE) || !function) return EINVAL;
    pthread_attr_t attr;
    if (attributes) {
        if (nos_user_copy(&attr, attributes, sizeof(attr))) return EINVAL;
        if (!attr.is_initialized) return EINVAL;
        if (attr.detachstate != PTHREAD_CREATE_JOINABLE || attr.inheritsched != PTHREAD_INHERIT_SCHED)
            return ENOTSUP;
    } else pthread_attr_init(&attr);
    nos_lock_acquire(&object_lock);
    unsigned index;
    for (index = 0; index < NOS_TASKS; index++) if (!threads[index].used) break;
    if (index == NOS_TASKS) { nos_lock_release(&object_lock); return EAGAIN; }
    struct thread_record *record = &threads[index];
    record->used = 1; record->joining = 0; record->function = function; record->argument = argument;
    pthread_t handle = 0x80000000u | (record->generation * NOS_TASKS + index);
    int rc = nos_thread_create_with_stack(&record->native, thread_entry, record,
        attr.stackaddr, (size_t)attr.stacksize);
    if (rc) record->used = 0;
    nos_lock_release(&object_lock);
    if (rc) return error(rc);
    *output = handle; return 0;
}
int pthread_join(pthread_t handle, void **value)
{
    nos_lock_acquire(&object_lock);
    struct thread_record *record = thread_record(handle);
    if (!record || record->joining) { nos_lock_release(&object_lock); return EINVAL; }
    if ((unsigned)record->native.tid == tid()) { nos_lock_release(&object_lock); return EDEADLK; }
    record->joining = 1;
    nos_lock_release(&object_lock);
    int rc = nos_thread_join(&record->native, value);
    nos_lock_acquire(&object_lock);
    if (!rc) { record->used = 0; record->generation++; }
    else record->joining = 0;
    nos_lock_release(&object_lock); return error(rc);
}
void pthread_exit(void *value)
{
    unsigned identifier = tid();
    for (unsigned i = 0; i < NOS_TASKS; i++) {
        if (threads[i].used && (unsigned)threads[i].native.tid == identifier) {
            __atomic_store_n(&threads[i].native.result, value, __ATOMIC_RELEASE); break;
        }
    }
    local_cleanup(); nos_process_exit(0);
}
pthread_t pthread_self(void)
{
    unsigned identifier = tid();
    for (unsigned i = 0; i < NOS_TASKS; i++)
        if (threads[i].used && (unsigned)threads[i].native.tid == identifier)
            return 0x80000000u | (threads[i].generation * NOS_TASKS + i);
    return 0x40000000u | identifier;
}
int pthread_equal(pthread_t first, pthread_t second) { return first == second; }
int pthread_detach(pthread_t thread) { (void)thread; return ENOTSUP; }
int pthread_cancel(pthread_t thread) { (void)thread; return ENOTSUP; }
int pthread_attr_init(pthread_attr_t *attr)
{
    if (!attr) return EINVAL;
    u_memset(attr, 0, sizeof(*attr)); attr->is_initialized = 1;
    attr->stacksize = 8192; attr->contentionscope = PTHREAD_SCOPE_SYSTEM;
    attr->inheritsched = PTHREAD_INHERIT_SCHED; attr->detachstate = PTHREAD_CREATE_JOINABLE;
    return 0;
}
int pthread_attr_destroy(pthread_attr_t *attr) { if (!attr) return EINVAL; attr->is_initialized = 0; return 0; }
int pthread_attr_setstacksize(pthread_attr_t *attr, size_t size)
{ if (!attr || !attr->is_initialized || size < 4096 || size > 65536 || size % 16) return EINVAL;
    attr->stacksize = (int)size; return 0; }
int pthread_attr_getstacksize(const pthread_attr_t *attr, size_t *size)
{ if (!attr || !size || !attr->is_initialized) return EINVAL; *size = (size_t)attr->stacksize; return 0; }
int pthread_attr_setstack(pthread_attr_t *attr, void *stack, size_t size)
{ int rc = pthread_attr_setstacksize(attr, size); if (!rc) attr->stackaddr = stack; return rc; }
int pthread_attr_getstack(const pthread_attr_t *attr, void **stack, size_t *size)
{ int rc = pthread_attr_getstacksize(attr, size); if (!rc) *stack = attr->stackaddr; return rc; }
int pthread_attr_setdetachstate(pthread_attr_t *attr, int state)
{ if (!attr || !attr->is_initialized) return EINVAL; if (state != PTHREAD_CREATE_JOINABLE) return ENOTSUP;
    attr->detachstate = state; return 0; }
int pthread_attr_getdetachstate(const pthread_attr_t *attr, int *state)
{ if (!attr || !state || !attr->is_initialized) return EINVAL; *state = attr->detachstate; return 0; }
int pthread_attr_setguardsize(pthread_attr_t *attr, size_t size)
{ return !attr || !attr->is_initialized ? EINVAL : size ? ENOTSUP : 0; }
int pthread_attr_getguardsize(const pthread_attr_t *attr, size_t *size)
{ if (!attr || !size || !attr->is_initialized) return EINVAL; *size = 0; return 0; }

static int mutex_allocate(pthread_mutex_t *output, int type)
{
    for (unsigned i = 0; i < POSIX_OBJECTS; i++) if (!mutexes[i].used) {
        struct mutex_record *record = &mutexes[i];
        record->used = 1; record->owner = 0; record->depth = 0; record->waiters = 0; record->type = type;
        *output = record->generation * POSIX_OBJECTS + i; return 0;
    }
    return EAGAIN;
}
static struct mutex_record *mutex_record(pthread_mutex_t *mutex)
{
    if (!mutex) return NULL;
    nos_lock_acquire(&object_lock);
    if (*mutex == LAZY_OBJECT && mutex_allocate(mutex, PTHREAD_MUTEX_NORMAL)) {
        nos_lock_release(&object_lock); return NULL;
    }
    struct mutex_record *record = &mutexes[*mutex % POSIX_OBJECTS];
    int valid = record->used && record->generation == *mutex / POSIX_OBJECTS;
    nos_lock_release(&object_lock); return valid ? record : NULL;
}
int pthread_mutexattr_init(pthread_mutexattr_t *attr)
{ if (!attr) return EINVAL; u_memset(attr, 0, sizeof(*attr)); attr->is_initialized = 1; attr->type = PTHREAD_MUTEX_NORMAL; return 0; }
int pthread_mutexattr_destroy(pthread_mutexattr_t *attr)
{ if (!attr) return EINVAL; attr->is_initialized = 0; return 0; }
int pthread_mutexattr_settype(pthread_mutexattr_t *attr, int type)
{ if (!attr || !attr->is_initialized || type < PTHREAD_MUTEX_NORMAL || type > PTHREAD_MUTEX_DEFAULT) return EINVAL;
    attr->type = type; return 0; }
int pthread_mutexattr_gettype(const pthread_mutexattr_t *attr, int *type)
{ if (!attr || !type || !attr->is_initialized) return EINVAL; *type = attr->type; return 0; }
int pthread_mutex_init(pthread_mutex_t *mutex, const pthread_mutexattr_t *attr)
{ if (!mutex || (attr && !attr->is_initialized)) return EINVAL;
    nos_lock_acquire(&object_lock); int rc = mutex_allocate(mutex, attr ? attr->type : PTHREAD_MUTEX_NORMAL);
    nos_lock_release(&object_lock); return rc; }
int pthread_mutex_destroy(pthread_mutex_t *mutex)
{
    struct mutex_record *record = mutex_record(mutex); if (!record) return EINVAL;
    if (__atomic_load_n(&record->owner, __ATOMIC_ACQUIRE) ||
        __atomic_load_n(&record->waiters, __ATOMIC_ACQUIRE)) return EBUSY;
    nos_lock_acquire(&object_lock); record->used = 0; record->generation++; *mutex = LAZY_OBJECT - 1;
    nos_lock_release(&object_lock); return 0;
}
static int mutex_acquire(pthread_mutex_t *mutex, int try)
{
    struct mutex_record *record = mutex_record(mutex); if (!record) return EINVAL;
    int owner = (int)(tid() + 1u);
    for (;;) {
        int expected = 0;
        if (__atomic_compare_exchange_n(&record->owner, &expected, owner, 0,
            __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) { record->depth = 1; return 0; }
        if (expected == owner && record->type == PTHREAD_MUTEX_RECURSIVE) { record->depth++; return 0; }
        if (expected == owner && record->type == PTHREAD_MUTEX_ERRORCHECK) return EDEADLK;
        if (try) return EBUSY;
        __atomic_add_fetch(&record->waiters, 1, __ATOMIC_ACQ_REL);
        intptr_t rc = nos_syscall(NOS_SYS_FUTEX_WAIT, (uintptr_t)&record->owner, (unsigned)expected, 0);
        __atomic_sub_fetch(&record->waiters, 1, __ATOMIC_RELEASE);
        if (rc < 0 && rc != -NOS_EAGAIN && rc != -NOS_EINTR) return error((int)rc);
    }
}
int pthread_mutex_lock(pthread_mutex_t *mutex) { return mutex_acquire(mutex, 0); }
int pthread_mutex_trylock(pthread_mutex_t *mutex) { return mutex_acquire(mutex, 1); }
int pthread_mutex_unlock(pthread_mutex_t *mutex)
{
    struct mutex_record *record = mutex_record(mutex); if (!record) return EINVAL;
    if (__atomic_load_n(&record->owner, __ATOMIC_ACQUIRE) != (int)(tid() + 1u)) return EPERM;
    if (--record->depth) return 0;
    __atomic_store_n(&record->owner, 0, __ATOMIC_RELEASE);
    nos_syscall(NOS_SYS_FUTEX_WAKE, (uintptr_t)&record->owner, 1, 0); return 0;
}
static int condition_allocate(pthread_cond_t *output, clockid_t clock)
{
    for (unsigned i = 0; i < POSIX_OBJECTS; i++) if (!conditions[i].used) {
        struct condition_record *record = &conditions[i];
        record->used = 1; record->waiters = 0; record->sequence = 0; record->clock = clock;
        *output = record->generation * POSIX_OBJECTS + i; return 0;
    }
    return EAGAIN;
}
static struct condition_record *condition_record(pthread_cond_t *condition)
{
    if (!condition) return NULL;
    nos_lock_acquire(&object_lock);
    if (*condition == LAZY_OBJECT && condition_allocate(condition, CLOCK_REALTIME)) {
        nos_lock_release(&object_lock); return NULL;
    }
    struct condition_record *record = &conditions[*condition % POSIX_OBJECTS];
    int valid = record->used && record->generation == *condition / POSIX_OBJECTS;
    nos_lock_release(&object_lock); return valid ? record : NULL;
}
int pthread_condattr_init(pthread_condattr_t *attr)
{ if (!attr) return EINVAL; u_memset(attr, 0, sizeof(*attr)); attr->is_initialized = 1; attr->clock = CLOCK_REALTIME; return 0; }
int pthread_condattr_destroy(pthread_condattr_t *attr) { if (!attr) return EINVAL; attr->is_initialized = 0; return 0; }
int pthread_condattr_setclock(pthread_condattr_t *attr, clockid_t clock)
{ if (!attr || !attr->is_initialized) return EINVAL; if (clock != CLOCK_MONOTONIC && clock != CLOCK_REALTIME) return EINVAL;
    attr->clock = clock; return 0; }
int pthread_condattr_getclock(const pthread_condattr_t *attr, clockid_t *clock)
{ if (!attr || !clock || !attr->is_initialized) return EINVAL; *clock = attr->clock; return 0; }
int pthread_cond_init(pthread_cond_t *condition, const pthread_condattr_t *attr)
{ if (!condition || (attr && !attr->is_initialized)) return EINVAL;
    nos_lock_acquire(&object_lock); int rc = condition_allocate(condition, attr ? attr->clock : CLOCK_REALTIME);
    nos_lock_release(&object_lock); return rc; }
int pthread_cond_destroy(pthread_cond_t *condition)
{ struct condition_record *record = condition_record(condition); if (!record) return EINVAL;
    if (__atomic_load_n(&record->waiters, __ATOMIC_ACQUIRE)) return EBUSY;
    nos_lock_acquire(&object_lock); record->used = 0; record->generation++; *condition = LAZY_OBJECT - 1;
    nos_lock_release(&object_lock); return 0; }
static int condition_wait(pthread_cond_t *condition, pthread_mutex_t *mutex, const struct timespec *absolute)
{
    struct condition_record *record = condition_record(condition); if (!record) return EINVAL;
    uintptr_t timeout = 0;
    if (absolute) {
        if (absolute->tv_sec < 0 || absolute->tv_nsec < 0 || absolute->tv_nsec >= 1000000000L) return EINVAL;
        struct timespec now;
        if (clock_gettime(record->clock, &now)) return errno;
        int64_t seconds = (int64_t)absolute->tv_sec - now.tv_sec;
        int64_t ns = (int64_t)absolute->tv_nsec - now.tv_nsec;
        if (seconds < 0 || (seconds == 0 && ns <= 0)) return ETIMEDOUT;
        if (seconds > 1000) return ENOTSUP;
        int64_t total = seconds * 100 + (ns + 9999999) / 10000000;
        timeout = total > 0 ? (uintptr_t)total : 1;
    }
    int sequence = __atomic_load_n(&record->sequence, __ATOMIC_ACQUIRE);
    __atomic_add_fetch(&record->waiters, 1, __ATOMIC_ACQ_REL);
    int rc = pthread_mutex_unlock(mutex);
    if (!rc) {
        intptr_t waited = nos_syscall(NOS_SYS_FUTEX_WAIT, (uintptr_t)&record->sequence, (unsigned)sequence, timeout);
        rc = waited == -NOS_EAGAIN || waited == -NOS_EINTR ? 0 : error((int)waited);
        int locked = pthread_mutex_lock(mutex); if (locked) rc = locked;
    }
    __atomic_sub_fetch(&record->waiters, 1, __ATOMIC_RELEASE); return rc;
}
int pthread_cond_wait(pthread_cond_t *condition, pthread_mutex_t *mutex) { return condition_wait(condition, mutex, NULL); }
int pthread_cond_timedwait(pthread_cond_t *condition, pthread_mutex_t *mutex, const struct timespec *time)
{ return time ? condition_wait(condition, mutex, time) : EINVAL; }
static int condition_wake(pthread_cond_t *condition, unsigned count)
{ struct condition_record *record = condition_record(condition); if (!record) return EINVAL;
    __atomic_add_fetch(&record->sequence, 1, __ATOMIC_RELEASE);
    intptr_t rc = nos_syscall(NOS_SYS_FUTEX_WAKE, (uintptr_t)&record->sequence, count, 0);
    return rc < 0 ? error((int)rc) : 0; }
int pthread_cond_signal(pthread_cond_t *condition) { return condition_wake(condition, 1); }
int pthread_cond_broadcast(pthread_cond_t *condition) { return condition_wake(condition, NOS_TASKS); }
int pthread_key_create(pthread_key_t *key, void (*destructor)(void *))
{ if (!key) return EINVAL; nos_lock_acquire(&object_lock);
    for (unsigned i = 0; i < POSIX_KEYS; i++) if (!keys[i].used) {
        keys[i].used = 1; keys[i].destructor = destructor; *key = keys[i].generation * POSIX_KEYS + i;
        nos_lock_release(&object_lock); return 0; }
    nos_lock_release(&object_lock); return EAGAIN; }
int pthread_key_delete(pthread_key_t key)
{ unsigned index = key % POSIX_KEYS; nos_lock_acquire(&object_lock);
    if (!keys[index].used || keys[index].generation != key / POSIX_KEYS) { nos_lock_release(&object_lock); return EINVAL; }
    keys[index].used = 0; keys[index].generation++;
    for (unsigned i = 0; i < NOS_TASKS; i++) locals[i].values[index] = NULL;
    nos_lock_release(&object_lock); return 0; }
int pthread_setspecific(pthread_key_t key, const void *value)
{ unsigned index = key % POSIX_KEYS;
    if (!keys[index].used || keys[index].generation != key / POSIX_KEYS) return EINVAL;
    locals[local_index()].values[index] = (void *)value; return 0; }
void *pthread_getspecific(pthread_key_t key)
{ unsigned index = key % POSIX_KEYS;
    return keys[index].used && keys[index].generation == key / POSIX_KEYS ? locals[local_index()].values[index] : NULL; }
int pthread_once(pthread_once_t *once, void (*function)(void))
{
    if (!once || !once->is_initialized || !function) return EINVAL;
    int expected = 0;
    if (__atomic_compare_exchange_n(&once->init_executed, &expected, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        function(); __atomic_store_n(&once->init_executed, 1, __ATOMIC_RELEASE);
        nos_syscall(NOS_SYS_FUTEX_WAKE, (uintptr_t)&once->init_executed, NOS_TASKS, 0);
    } else while (__atomic_load_n(&once->init_executed, __ATOMIC_ACQUIRE) == 2)
        nos_syscall(NOS_SYS_FUTEX_WAIT, (uintptr_t)&once->init_executed, 2, 0);
    return 0;
}
int pthread_setcancelstate(int state, int *old)
{ if (state != PTHREAD_CANCEL_ENABLE && state != PTHREAD_CANCEL_DISABLE) return EINVAL;
    unsigned index = local_index(); if (old) *old = locals[index].cancel_state;
    locals[index].cancel_state = state; return 0; }
int pthread_setcanceltype(int type, int *old)
{ if (type != PTHREAD_CANCEL_DEFERRED) return ENOTSUP; if (old) *old = PTHREAD_CANCEL_DEFERRED; return 0; }
void pthread_testcancel(void) { }
void _pthread_cleanup_push(struct _pthread_cleanup_context *context, void (*function)(void *), void *argument)
{ unsigned index = local_index(); context->_routine = function; context->_arg = argument;
    context->_previous = locals[index].cleanup; locals[index].cleanup = context; }
void _pthread_cleanup_pop(struct _pthread_cleanup_context *context, int execute)
{ unsigned index = local_index(); if (locals[index].cleanup == context) locals[index].cleanup = context->_previous;
    if (execute) context->_routine(context->_arg); }
