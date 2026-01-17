/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ASM_GENERIC_SWITCH_H__
#define __ASM_GENERIC_SWITCH_H__

#include <kernel/types.h>

/* 通用上下文切换抽象接口 */

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

#endif /* __ASM_GENERIC_SWITCH_H__ */