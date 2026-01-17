/**
 * Copyright (C) 2023-2024 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ASM_GENERIC_ATOMIC_H__
#define __ASM_GENERIC_ATOMIC_H__

#include <kernel/types.h>

/* 通用原子操作抽象接口 */

typedef struct {
    volatile int counter;
} atomic_t;

/**
 * 原子读取
 * @v: 原子变量指针
 * @return: 当前值
 */
static inline int atomic_read(const atomic_t *v)
{
    return v->counter;
}

/**
 * 原子设置
 * @v: 原子变量指针
 * @i: 要设置的值
 */
static inline void atomic_set(atomic_t *v, int i)
{
    v->counter = i;
}

/**
 * 原子加法
 * @i: 要加的值
 * @v: 原子变量指针
 */
void atomic_add(int i, atomic_t *v);

/**
 * 原子减法
 * @i: 要减的值
 * @v: 原子变量指针
 */
void atomic_sub(int i, atomic_t *v);

/**
 * 原子自增
 * @v: 原子变量指针
 */
void atomic_inc(atomic_t *v);

/**
 * 原子自减
 * @v: 原子变量指针
 */
void atomic_dec(atomic_t *v);

/**
 * 原子比较交换
 * @v: 原子变量指针
 * @old: 期望的旧值
 * @new: 要设置的新值
 * @return: 实际的旧值
 */
int atomic_cmpxchg(atomic_t *v, int old, int new);

/**
 * 原子测试并设置
 * @v: 原子变量指针
 * @return: 之前的值
 */
int atomic_test_and_set(atomic_t *v);

#endif /* __ASM_GENERIC_ATOMIC_H__ */