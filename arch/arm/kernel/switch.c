/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <asm/switch.h>
#include <kernel/task.h>
#include <kernel/printk.h>

/* ARM Cortex-M 系列上下文结构 */
struct cpu_context {
    unsigned long r4;
    unsigned long r5;
    unsigned long r6;
    unsigned long r7;
    unsigned long r8;
    unsigned long r9;
    unsigned long r10;
    unsigned long r11;
    unsigned long sp;
    unsigned long lr;
};

/**
 * 执行任务上下文切换 (汇编实现)
 */
extern void __arch_switch_to(struct cpu_context *prev, struct cpu_context *next);

/**
 * 执行任务上下文切换
 * @prev: 当前任务
 * @next: 要切换到的任务
 */
void arch_switch_to(struct task_struct *prev, struct task_struct *next)
{
    if (prev == next) {
        return;
    }
    
    /* 调用汇编实现的上下文切换 */
    __arch_switch_to((struct cpu_context *)prev->cpu_context,
                     (struct cpu_context *)next->cpu_context);
}

/**
 * 初始化任务上下文
 * @task: 任务结构
 * @entry: 任务入口函数
 * @stack: 任务栈指针
 */
void arch_task_init(struct task_struct *task, void (*entry)(void), void *stack)
{
    struct cpu_context *context = (struct cpu_context *)task->cpu_context;
    
    /* 清零上下文 */
    for (int i = 0; i < sizeof(struct cpu_context) / sizeof(unsigned long); i++) {
        ((unsigned long *)context)[i] = 0;
    }
    
    /* 设置栈指针和返回地址 */
    context->sp = (unsigned long)stack;
    context->lr = (unsigned long)entry;
    
    pr_debug("Task context initialized: sp=0x%lx, entry=0x%lx\n", 
             context->sp, context->lr);
}

/**
 * 获取当前栈指针
 * @return: 当前栈指针
 */
unsigned long arch_get_sp(void)
{
    unsigned long sp;
    
    __asm__ volatile ("mov %0, sp" : "=r" (sp));
    
    return sp;
}

/**
 * 设置栈指针
 * @sp: 新的栈指针
 */
void arch_set_sp(unsigned long sp)
{
    __asm__ volatile ("mov sp, %0" :: "r" (sp));
}

/**
 * 保存当前 CPU 上下文
 * @context: 上下文保存缓冲区
 */
void arch_save_context(void *context)
{
    struct cpu_context *ctx = (struct cpu_context *)context;
    
    __asm__ volatile (
        "str r4, [%0, #0]\n"
        "str r5, [%0, #4]\n"
        "str r6, [%0, #8]\n"
        "str r7, [%0, #12]\n"
        "str r8, [%0, #16]\n"
        "str r9, [%0, #20]\n"
        "str r10, [%0, #24]\n"
        "str r11, [%0, #28]\n"
        "str sp, [%0, #32]\n"
        "str lr, [%0, #36]\n"
        :
        : "r" (ctx)
        : "memory"
    );
}

/**
 * 恢复 CPU 上下文
 * @context: 上下文缓冲区
 */
void arch_restore_context(void *context)
{
    struct cpu_context *ctx = (struct cpu_context *)context;
    
    __asm__ volatile (
        "ldr r4, [%0, #0]\n"
        "ldr r5, [%0, #4]\n"
        "ldr r6, [%0, #8]\n"
        "ldr r7, [%0, #12]\n"
        "ldr r8, [%0, #16]\n"
        "ldr r9, [%0, #20]\n"
        "ldr r10, [%0, #24]\n"
        "ldr r11, [%0, #28]\n"
        "ldr sp, [%0, #32]\n"
        "ldr lr, [%0, #36]\n"
        :
        : "r" (ctx)
        : "r4", "r5", "r6", "r7", "r8", "r9", "r10", "r11", "sp", "lr", "memory"
    );
}