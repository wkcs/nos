/* Copyright (C) 2026 NOS contributors */
#include "kernel.h"
#define IRQ_OBJECTS 8u
#define IRQ_BASE 0x20000000u
static struct irq_object { unsigned used, generation, irq, group, pending; } irq_objects[IRQ_OBJECTS];
static unsigned handle_of(unsigned index)
{ return IRQ_BASE + irq_objects[index].generation * IRQ_OBJECTS + index; }
static struct irq_object *lookup(uintptr_t handle, unsigned *index)
{
    if (handle < IRQ_BASE || handle >= NOS_ENDPOINT_BASE) return NULL;
    unsigned i = ((unsigned)handle - IRQ_BASE) % IRQ_OBJECTS;
    if (irq_objects[i].used != 1 || handle_of(i) != handle) return NULL;
    *index = i; return &irq_objects[i];
}
static void release(unsigned index)
{
    struct irq_object *o = &irq_objects[index]; arch_irq_mask(o->irq, 1);
    unsigned handle = handle_of(index);
    for (unsigned n = 0; n < NOS_TASKS; n++) {
        struct task *t = &tasks[n];
        if (t->state == TASK_IRQ && t->wait_for == handle) {
            t->state = TASK_READY; FRAME_RESULT(&t->frame) = (uintptr_t)-NOS_ESRCH;
        }
    }
    o->pending = 0;
    if (o->generation < (NOS_ENDPOINT_BASE - 1u - IRQ_BASE - index) / IRQ_OBJECTS) {
        o->generation++; o->used = 0;
    } else o->used = 2;
}
intptr_t irq_operation(unsigned nr, uintptr_t a, uintptr_t b)
{
    if (!(current_task->caps & NOS_CAP_DEVICE)) return -NOS_EACCES;
    if (nr == NOS_SYS_IRQ_BIND) {
        if (a > UINT32_MAX) return -NOS_EINVAL;
        unsigned i;
        for (i = 0; i < IRQ_OBJECTS && irq_objects[i].used; i++) { }
        if (i == IRQ_OBJECTS) return -NOS_ENOSPC;
        int irq = arch_irq_bind((unsigned)a);
        if (irq <= 0) return irq < 0 ? irq : -NOS_ENOTSUP;
        for (unsigned j = 0; j < IRQ_OBJECTS; j++) {
            if (irq_objects[j].used == 1 && irq_objects[j].irq == (unsigned)irq) return -NOS_EBUSY;
        }
        irq_objects[i].used = 1; irq_objects[i].irq = (unsigned)irq;
        irq_objects[i].group = current_task->space_owner; irq_objects[i].pending = 0;
        return handle_of(i);
    }
    unsigned i; struct irq_object *o = lookup(a, &i);
    if (!o) return -NOS_EINVAL;
    if (o->group != current_task->space_owner) return -NOS_EACCES;
    if (nr == NOS_SYS_IRQ_RELEASE) { release(i); reschedule = 1; return 0; }
    if (nr == NOS_SYS_IRQ_ACK) { arch_irq_mask(o->irq, 0); return 0; }
    if (nr != NOS_SYS_IRQ_WAIT || b > 100000) return -NOS_EINVAL;
    if (o->pending) { unsigned pending = o->pending; o->pending = 0; return pending; }
    for (unsigned n = 0; n < NOS_TASKS; n++)
        if (tasks[n].state == TASK_IRQ && tasks[n].wait_for == a) return -NOS_EBUSY;
    current_task->wait_for = (unsigned)a;
    current_task->wake_tick = b ? kernel_ticks + b : 0;
    current_task->state = TASK_IRQ; reschedule = 1; return 0;
}
void kernel_irq_notify(unsigned irq)
{
    for (unsigned i = 0; i < IRQ_OBJECTS; i++) {
        struct irq_object *o = &irq_objects[i];
        if (o->used != 1 || o->irq != irq) continue;
        if (o->pending != UINT32_MAX) o->pending++;
        for (unsigned n = 0; n < NOS_TASKS; n++) {
            struct task *t = &tasks[n];
            if (t->state == TASK_IRQ && t->wait_for == handle_of(i)) {
                t->state = TASK_READY; FRAME_RESULT(&t->frame) = o->pending; o->pending = 0;
            }
        }
        reschedule = 1; return;
    }
}
void irq_timeouts(void)
{
    for (unsigned i = 0; i < NOS_TASKS; i++) {
        struct task *t = &tasks[i];
        if (t->state == TASK_IRQ && t->wake_tick && t->wake_tick <= kernel_ticks) {
            t->state = TASK_READY; FRAME_RESULT(&t->frame) = (uintptr_t)-NOS_ETIMEDOUT;
        }
    }
}
void irq_task_exit(struct task *task)
{
    for (unsigned n = 0; n < NOS_TASKS; n++) {
        struct task *peer = &tasks[n];
        if (peer != task && peer->space_owner == task->space_owner && peer->state != TASK_UNUSED &&
            peer->state != TASK_DEAD && peer->state != TASK_RETIRED) return;
    }
    for (unsigned i = 0; i < IRQ_OBJECTS; i++)
        if (irq_objects[i].used == 1 && irq_objects[i].group == task->space_owner) release(i);
}
