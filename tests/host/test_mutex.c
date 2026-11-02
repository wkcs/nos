/** Copyright (C) 2026 Nick Hu <huqihan@live.com> */
#include <kernel/mutex.h>
#include <stdio.h>

static struct mutex mutex;
static struct task_struct owner, waiter;
static struct mutex m1, m2;
static struct task_struct t_low, t_mid, t_other, t_high;
struct task_struct *current;
static int switched, resumed;
static int switch_calls;
static int simulate_timeout_mode;

static void init_task(struct task_struct *t, const char *name, uint8_t prio)
{
    INIT_LIST_HEAD(&t->list);
    INIT_LIST_HEAD(&t->held_mutex_list);
    t->blocked_on = NULL;
    spin_lock_init(&t->lock);
    t->list_lock = NULL;
    t->timer.ticks = 0;
    t->timer.active = 0;
    t->name = name;
    t->wchan_name = NULL;
    t->wchan_addr = 0;
    t->current_priority = prio;
    t->init_priority = prio;
    t->pi_boost_prio = TASK_PRIO_INVALID;
    t->flag = 0;
}

void timer_start(struct timer *timer, uint32_t tick)
{
    timer->ticks = tick;
    timer->active = 1;
}

int task_pi_boost(struct task_struct *t, uint8_t waiter_prio)
{
    spin_lock_irq(&t->lock);
    if (waiter_prio < t->pi_boost_prio) {
        t->pi_boost_prio = waiter_prio;
    }
    t->current_priority = (t->pi_boost_prio < t->init_priority) ?
                          t->pi_boost_prio : t->init_priority;
    spin_unlock_irq(&t->lock);
    return 0;
}

int task_pi_unboost(struct task_struct *t, uint8_t next_waiter_prio)
{
    uint8_t best = next_waiter_prio;
    struct mutex *m;

    spin_lock_irq(&t->lock);
    list_for_each_entry(m, &t->held_mutex_list, held_node) {
        if (!list_empty(&m->wait_list)) {
            struct task_struct *w = list_first_entry(&m->wait_list, struct task_struct, list);
            if (w->current_priority < best) {
                best = w->current_priority;
            }
        }
    }
    t->pi_boost_prio = best;
    t->current_priority = (t->pi_boost_prio < t->init_priority) ?
                          t->pi_boost_prio : t->init_priority;
    spin_unlock_irq(&t->lock);
    return 0;
}

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
    assert(task->lock.held);
    return 0;
}
int task_resume(struct task_struct *task)
{
    (void)task;
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
    switch_calls++;
    if (simulate_timeout_mode == 1 && current == &t_high) {
        /* Verify Chain PI state right before timeout expires */
        assert(t_mid.current_priority == 1);
        assert(t_low.current_priority == 1);
        assert(list_first_entry(&m2.wait_list, struct task_struct, list) == &t_mid);
        /* Simulate timer expiration waking t_high with -ETIMEDOUT */
        t_high.flag = -ETIMEDOUT;
        simulate_timeout_mode = 0;
        return;
    }
    if (current == &waiter && !switched) {
        switched = 1;
        current = &owner;
        mutex_unlock(&mutex);
        current = &waiter;
    }
}
int main(void)
{
    int before_switches;
    int rc;

    init_task(&owner, "owner", 5);
    init_task(&waiter, "waiter", 3);
    mutex_init(&mutex);
    current = NULL;
    mutex_lock(&mutex);
    assert(mutex_is_locked(&mutex) && mutex.owner == NULL);
    mutex_unlock(&mutex);
    assert(!mutex_is_locked(&mutex) && mutex.owner == NULL);

    /* Uncontended lock + unlock fast-path must NOT call switch_task() */
    current = &owner;
    before_switches = switch_calls;
    mutex_lock(&mutex);
    assert(mutex_is_locked(&mutex) && mutex.owner == &owner);
    /* Recursive self-deadlock detection must return -EDEADLK */
    assert(mutex_lock_timeout(&mutex, 0U) == -EDEADLK);
    /* Try-lock while held must return -EBUSY */
    assert(mutex_try_lock(&mutex) == -EBUSY);
    mutex_unlock(&mutex);
    assert(!mutex_is_locked(&mutex) && mutex.owner == NULL);
    assert(switch_calls == before_switches);

    current = &owner;
    mutex_lock(&mutex);
    current = &waiter;
    mutex_lock(&mutex);
    assert(switched && resumed == 1);
    assert(mutex.locked && mutex.owner == &waiter);
    assert(owner.current_priority == owner.init_priority);
    /* Waiter was not PI-boosted and wait_list is empty -> fast-path unlock without extra switch_task() */
    before_switches = switch_calls;
    mutex_unlock(&mutex);
    assert(!mutex.locked && mutex.owner == NULL);
    assert(switch_calls == before_switches);

    /*
     * Test 2-hop Chain PI + Wait-List Re-sorting + Timeout PI Rollback:
     *   t_low (prio 10) holds m2
     *   t_other (prio 4) waits on m2 -> boosts t_low to prio 4
     *   t_mid (prio 6) holds m1 and waits on m2 (queued behind t_other)
     *   t_high (prio 1) calls mutex_lock_timeout(&m1, 5):
     *     -> boosts t_mid from 6 to 1
     *     -> re-sorts t_mid ahead of t_other (4) in m2.wait_list
     *     -> boosts t_low from 4 to 1
     *   Then t_high times out (-ETIMEDOUT):
     *     -> t_mid unboosts back to 6
     *     -> t_mid is re-sorted behind t_other (4) in m2.wait_list
     *     -> t_low unboosts from 1 back to 4 (t_other's priority)
     */
    init_task(&t_low, "low", 10);
    init_task(&t_mid, "mid", 6);
    init_task(&t_other, "other", 4);
    init_task(&t_high, "high", 1);
    mutex_init(&m1);
    mutex_init(&m2);

    current = &t_low;
    mutex_lock(&m2);
    current = &t_mid;
    mutex_lock(&m1);

    /* Manually place t_other (prio 4) and t_mid (prio 6) as blocked waiters on m2 */
    list_add_tail(&t_other.list, &m2.wait_list);
    t_other.list_lock = &m2.lock;
    t_other.blocked_on = &m2;
    task_pi_boost(&t_low, t_other.current_priority);
    assert(t_low.current_priority == 4);

    list_add_tail(&t_mid.list, &m2.wait_list);
    t_mid.list_lock = &m2.lock;
    t_mid.blocked_on = &m2;
    assert(list_first_entry(&m2.wait_list, struct task_struct, list) == &t_other);

    current = &t_high;
    simulate_timeout_mode = 1;
    rc = mutex_lock_timeout(&m1, 5U);
    assert(rc == -ETIMEDOUT);
    assert(t_high.timer.active == 1 && t_high.timer.ticks == 5U);
    assert(list_empty(&m1.wait_list));
    assert(t_mid.current_priority == 6);
    assert(list_first_entry(&m2.wait_list, struct task_struct, list) == &t_other);
    assert(t_low.current_priority == 4);

    puts("mutex unlock and waiter handoff checks passed");
    return 0;
}
