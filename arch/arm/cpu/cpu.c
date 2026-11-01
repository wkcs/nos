/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include <kernel/task.h>
#include <kernel/unwind.h>

void asm_cpu_set_lpm(void)
{
    asm volatile ("wfi");
}

void arch_dump_stack(void)
{
    dump_task_stack(current, NULL);
}
