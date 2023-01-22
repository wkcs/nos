/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include <kernel/cpu.h>
#include <board/board.h>

struct cpu_reg {
    addr_t r4;
    addr_t r5;
    addr_t r6;
    addr_t r7;
    addr_t r8;
    addr_t r9;
    addr_t r10;
    addr_t r11;

    addr_t r0;
    addr_t r1;
    addr_t r2;
    addr_t r3;
    addr_t r12;
    addr_t lr;
    addr_t pc;
    addr_t psr;
};

struct cpu_dump_type {
    addr_t sp;
    struct cpu_reg cpu_reg;
};

void NMI_Handler(void)
{
}

void MemManage_Handler(void)
{
  /* Go to infinite loop when Memory Manage exception occurs */
  while (1)
  {
  }
}


void BusFault_Handler(void)
{
  /* Go to infinite loop when Bus Fault exception occurs */
  while (1)
  {
  }
}

void UsageFault_Handler(void)
{
  /* Go to infinite loop when Usage Fault exception occurs */
  while (1)
  {
  }
}

void SVC_Handler(void)
{
}

void DebugMon_Handler(void)
{
}

extern uint32_t SystemCoreClock;

static uint32_t sys_tick_num_by_us;
static uint32_t sys_tick_num_by_beat;
__init void asm_cpu_init(void)
{
    SysTick_Config(SystemCoreClock * CONFIG_SYS_TICK_MS / 1000);

    sys_tick_num_by_us = SystemCoreClock / 1000000;
    sys_tick_num_by_beat = SystemCoreClock * CONFIG_SYS_TICK_MS / 1000;
}

void asm_cpu_delay_us(uint32_t us)
{
    register uint32_t ticks;
    register uint32_t told, tnow;
    register uint32_t tcnt = 0;
    register uint32_t reload = SysTick->LOAD;

    ticks = us * sys_tick_num_by_us;
    tcnt = 0;

    told = SysTick->VAL;
    while (1) {
        tnow = SysTick->VAL;
        if (tnow != told) {
            if (tnow < told)
                tcnt += told - tnow;
            else
                tcnt += reload - tnow + told;
            told = tnow;
            if (tcnt >= ticks) {
                break;
            }
        }
    }
}

void SysTick_Handler(void)
{
    system_beat_processing();
}

void asm_cpu_reboot(void)
{
    __set_FAULTMASK(1);
    NVIC_SystemReset();
}

u64 asm_cpu_run_time_us(void)
{
    u64 us = cpu_run_ticks() * CONFIG_SYS_TICK_MS * 1000;
    return (us + (sys_tick_num_by_beat - SysTick->VAL) / sys_tick_num_by_us);
}
