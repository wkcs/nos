/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ARM_ASM_CPU_H__
#define __ARM_ASM_CPU_H__

#define USE_CPU_FFS
inline int __ffs(int value)
{
    return __builtin_ffs(value);
}

__init void asm_cpu_init(void);
void asm_cpu_delay_ns(uint32_t ns);
void asm_cpu_delay_us(uint32_t us);
void asm_cpu_reboot(void);
u64 asm_cpu_run_time_us(void);
addr_t *stack_init(void *task_entry, void *parameter, addr_t *stack_addr, void *task_exit);
void context_switch_interrupt(addr_t from, addr_t to);
void context_switch(addr_t from, addr_t to);
void context_switch_to(addr_t to);
void asm_cpu_set_lpm(void);

#endif /* __ARM_ASM_CPU_H__ */
