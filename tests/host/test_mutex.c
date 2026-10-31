/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <kernel/mutex.h>
#include <stdio.h>

static struct mutex mutex;
static struct task_struct owner, waiter;
struct task_struct *current;
static int switched, resumed;

int task_hang(struct task_struct *task)
{
    (void)task;
    /* Reproduce an unlock after the old lock check, before enqueueing. */
    current = &owner;
    mutex_unlock(&mutex);
    current = &waiter;
    return 0;
}
int task_hang_lock(struct task_struct *task)
{
    assert(task->lock.held && mutex.lock.held);
    return 0;
}
int task_resume(struct task_struct *task)
{
    assert(task == &waiter);
    resumed++;
    return 0;
}
int task_set_prio(struct task_struct *task, unsigned prio)
{
    task->current_priority = prio;
    return 0;
}
void switch_task(void)
{
    if (current == &waiter && !switched) {
        switched = 1;
        current = &owner;
        mutex_unlock(&mutex);
        current = &waiter;
    }
}
int main(void)
{
    INIT_LIST_HEAD(&owner.list);
    INIT_LIST_HEAD(&waiter.list);
    owner.current_priority = owner.init_priority = 5;
    waiter.current_priority = waiter.init_priority = 3;
    mutex_init(&mutex);
    current = NULL;
    mutex_lock(&mutex);
    assert(mutex.locked && mutex.owner == NULL);
    mutex_unlock(&mutex);
    assert(!mutex.locked && mutex.owner == NULL);

    current = &owner;
    mutex_lock(&mutex);
    current = &waiter;
    mutex_lock(&mutex);
    assert(switched && resumed == 1);
    assert(mutex.locked && mutex.owner == &waiter);
    mutex_unlock(&mutex);
    assert(!mutex.locked && mutex.owner == NULL);
    puts("mutex unlock and waiter handoff checks passed");
    return 0;
}
