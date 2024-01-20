/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __NOS_CPU_H__
#define __NOS_CPU_H__

#include <kernel/kernel.h>
#include <asm/cpu.h>

enum cpu_reboot_flag {
    REBOOT_TO_BOOTLOADER,
    REBOOT_TO_KERNEL_A,
    REBOOT_TO_KERNEL_B,
    REBOOT_TO_KERNEL = 0xffffffff,
};

__init void cpu_init(void);
void system_heartbeat_process(void);
u64 cpu_run_ticks(void);
u64 cpu_run_time_us(void);
void cpu_reboot(u32 flag);
void cpu_delay_ns(u32 ns);
void cpu_delay_us(u32 us);
#ifndef USE_CPU_FFS
int __ffs(int value);
#endif
void cpu_set_lpm(void);

#endif /* __NOS_CPU_H__ */
