/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 */

#ifndef __X86_ASM_SWITCH_H__
#define __X86_ASM_SWITCH_H__

struct task_struct;
void arch_switch_to(struct task_struct *prev, struct task_struct *next);
void arch_task_init(struct task_struct *task, void (*entry)(void), void *stack);

#endif /* __X86_ASM_SWITCH_H__ */
