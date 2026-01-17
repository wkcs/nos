/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <asm/atomic.h>
#include <asm/irq.h>

/* ARM Cortex-M 系列原子操作实现 (使用中断禁用) */

/**
 * 原子加法
 * @i: 要加的值
 * @v: 原子变量指针
 */
void atomic_add(int i, atomic_t *v)
{
    unsigned long flags;
    
    flags = arch_irq_save();
    v->counter += i;
    arch_irq_restore(flags);
}

/**
 * 原子减法
 * @i: 要减的值
 * @v: 原子变量指针
 */
void atomic_sub(int i, atomic_t *v)
{
    unsigned long flags;
    
    flags = arch_irq_save();
    v->counter -= i;
    arch_irq_restore(flags);
}

/**
 * 原子自增
 * @v: 原子变量指针
 */
void atomic_inc(atomic_t *v)
{
    atomic_add(1, v);
}

/**
 * 原子自减
 * @v: 原子变量指针
 */
void atomic_dec(atomic_t *v)
{
    atomic_sub(1, v);
}

/**
 * 原子比较交换
 * @v: 原子变量指针
 * @old: 期望的旧值
 * @new: 要设置的新值
 * @return: 实际的旧值
 */
int atomic_cmpxchg(atomic_t *v, int old, int new)
{
    unsigned long flags;
    int ret;
    
    flags = arch_irq_save();
    ret = v->counter;
    if (ret == old) {
        v->counter = new;
    }
    arch_irq_restore(flags);
    
    return ret;
}

/**
 * 原子测试并设置
 * @v: 原子变量指针
 * @return: 之前的值
 */
int atomic_test_and_set(atomic_t *v)
{
    return atomic_cmpxchg(v, 0, 1);
}