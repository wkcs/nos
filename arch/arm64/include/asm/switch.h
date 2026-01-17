/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ASM_ARM64_SWITCH_H__
#define __ASM_ARM64_SWITCH_H__

#include <kernel/types.h>

/* ARM64 架构上下文切换接口 */

struct task_struct;

/**
 * ARM64 CPU 上下文结构
 */
struct cpu_context {
    unsigned long x19;
    unsigned long x20;
    unsigned long x21;
    unsigned long x22;
    unsigned long x23;
    unsigned long x24;
    unsigned long x25;
    unsigned long x26;
    unsigned long x27;
    unsigned long x28;
    unsigned long fp;
    unsigned long sp;
    unsigned long pc;
};

/**
 * 执行任务上下文切换
 * @prev: 当前任务
 * @next: 要切换到的任务
 */
void arch_switch_to(struct task_struct *prev, struct task_struct *next);

/**
 * 初始化任务上下文
 * @task: 任务结构
 * @entry: 任务入口函数
 * @stack: 任务栈指针
 */
void arch_task_init(struct task_struct *task, void (*entry)(void), void *stack);

/**
 * 获取当前栈指针
 * @return: 当前栈指针
 */
unsigned long arch_get_sp(void);

/**
 * 设置栈指针
 * @sp: 新的栈指针
 */
void arch_set_sp(unsigned long sp);

#endif /* __ASM_ARM64_SWITCH_H__ */