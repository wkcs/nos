/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS 32 位原子引用计数 nos_refcount_t。
 */

#ifndef __NOS_REFCOUNT_H__
#define __NOS_REFCOUNT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <asm/atomic.h>

/**
 * 32 位原子引用计数结构体
 */
typedef struct nos_refcount {
    atomic_t ref;
} nos_refcount_t;

/**
 * 初始化引用计数
 *
 * @param ref: 引用计数指针
 * @param init_count: 初始计数值 (通常为 1)
 */
static inline void nos_refcount_init(struct nos_refcount *ref, uint32_t init_count)
{
    if (ref != NULL) {
        __atomic_store_n((uint32_t *)&ref->ref.counter, init_count, __ATOMIC_RELAXED);
    }
}

/**
 * 读取当前引用计数
 *
 * @param ref: 引用计数指针
 * @return: 当前计数值
 */
static inline uint32_t nos_refcount_read(const struct nos_refcount *ref)
{
    if (ref == NULL) {
        return 0U;
    }
    return __atomic_load_n((const uint32_t *)&ref->ref.counter, __ATOMIC_RELAXED);
}

/**
 * 增加引用计数 (Checked CAS)
 * 拒绝为 0 的对象复活，防止 UINT32_MAX 溢出回绕
 *
 * @param ref: 引用计数指针
 * @return: true 表示成功获取引用，false 表示对象已归零或溢出
 */
static inline bool nos_refcount_get(struct nos_refcount *ref)
{
    uint32_t val;

    if (ref == NULL) {
        return false;
    }

    val = __atomic_load_n((uint32_t *)&ref->ref.counter, __ATOMIC_RELAXED);
    while (val != 0U && val != UINT32_MAX) {
        if (__atomic_compare_exchange_n((uint32_t *)&ref->ref.counter,
                                         &val,
                                         val + 1U,
                                         true,
                                         __ATOMIC_ACQUIRE,
                                         __ATOMIC_RELAXED)) {
            return true;
        }
    }

    return false;
}

/**
 * 减少引用计数 (Checked CAS)
 * 拒绝下溢，并在计数降至 0 时返回 true
 *
 * @param ref: 引用计数指针
 * @return: true 表示计数降至 0 (需触发回收/退休流程)，false 表示仍有在途引用或原本即为 0
 */
static inline bool nos_refcount_put(struct nos_refcount *ref)
{
    uint32_t val;

    if (ref == NULL) {
        return false;
    }

    val = __atomic_load_n((uint32_t *)&ref->ref.counter, __ATOMIC_RELAXED);
    while (val != 0U) {
        if (__atomic_compare_exchange_n((uint32_t *)&ref->ref.counter,
                                         &val,
                                         val - 1U,
                                         true,
                                         __ATOMIC_RELEASE,
                                         __ATOMIC_RELAXED)) {
            if (val == 1U) {
                /* 降为 0，屏障确保析构代码能看到先前的写操作 */
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                return true;
            }
            return false;
        }
    }

    return false;
}

#endif /* __NOS_REFCOUNT_H__ */
