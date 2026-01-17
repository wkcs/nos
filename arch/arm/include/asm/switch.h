/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ASM_ARM_SWITCH_H__
#define __ASM_ARM_SWITCH_H__

#include <kernel/types.h>

/* ARM 架构上下文切换接口 */

struct task_struct;

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

/**
 * 保存当前 CPU 上下文
 * @context: 上下文保存缓冲区
 */
void arch_save_context(void *context);

/**
 * 恢复 CPU 上下文
 * @context: 上下文缓冲区
 */
void arch_restore_context(void *context);

#endif /* __ASM_ARM_SWITCH_H__ */