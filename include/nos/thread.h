/* Copyright (C) 2026 NOS contributors */
#ifndef NOS_THREAD_H
#define NOS_THREAD_H
#include "abi.h"
/* The object must remain alive until join. A thread shares memory and file
 * descriptors with its creator. Native APIs return negative NOS error codes. */
struct nos_thread {
    int tid;
    void *stack;
    size_t stack_size;
    void *(*function)(void *);
    void *argument;
    void *result;
    int owns_stack;
};
struct nos_lock { int word; };
struct nos_condition { int sequence; };
int nos_thread_create(struct nos_thread *thread, void *(*function)(void *), void *argument);
int nos_thread_create_with_stack(struct nos_thread *thread, void *(*function)(void *),
    void *argument, void *stack, size_t size);
int nos_thread_join(struct nos_thread *thread, void **result);
int nos_lock_acquire(struct nos_lock *lock);
int nos_lock_release(struct nos_lock *lock);
int nos_condition_wait(struct nos_condition *condition, struct nos_lock *lock);
int nos_condition_signal(struct nos_condition *condition, int all);
#endif
