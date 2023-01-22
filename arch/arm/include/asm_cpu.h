/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ASM_CPU_H__
#define __ASM_CPU_H__

#include <wk/kernel.h>
#include <board.h>

#define USE_CPU_FFS

extern uint32_t sys_tick_num_by_us;
extern uint32_t sys_tick_num_by_beat;

inline int __wk_ffs(int value)
{
    return __builtin_ffs(value);
}

inline void fatal_err(void)
{
    addr_t addr = 0xffffffff;

    asm  volatile ("mov pc, %0\n" : : "r" (addr) : "pc");
}

//获取cpu在当前节拍已经运行的时间
inline uint32_t get_cpu_run_time_by_sys_bead(void)
{
    return (sys_tick_num_by_beat - SysTick->VAL) / sys_tick_num_by_us;
}

extern addr_t *stack_init(void *task_entry, void *parameter, addr_t *stack_addr, void *task_exit);
extern addr_t disable_irq_save();
extern void enable_irq_save(addr_t level);
extern void context_switch_interrupt(addr_t from, addr_t to);
extern void context_switch(addr_t from, addr_t to);
extern void context_switch_to(addr_t to);
extern void asm_irq_init(void);
void asm_cpu_init(void);
void cpu_delay_usec(uint32_t usec);
void cpu_reboot(void);

#endif