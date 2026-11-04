/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS 通用对象生命周期管理头 (nos_object)。
 * 所有 IPC 原语结构体首部均嵌入 struct nos_object。
 */

#ifndef __NOS_OBJECT_H__
#define __NOS_OBJECT_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <kernel/spinlock.h>
#include <nos/refcount.h>

struct nos_object;

/**
 * 对象引用归零时的释放回调函数类型
 */
typedef void (*nos_object_release_t)(struct nos_object *object);

/**
 * 对象生命周期通用头部
 */
typedef struct nos_object {
    spinlock_t              lock;          /* 保护对象所有内部状态、队列与状态迁移 */
    nos_refcount_t          refcount;      /* 原子引用计数 */
    bool                    closed;        /* 是否已被关闭 */
    bool                    is_static;     /* 是否为静态声明内存 (不执行 kfree) */
    bool                    retired;       /* 是否已标记退休 (释放 owner 初始引用) */
    nos_object_release_t    release;       /* 引用归零时的释放回调 (可为 NULL) */
} nos_object_t;

/**
 * 初始化通用对象头部
 *
 * @param object: 目标对象指针
 * @param is_static: 是否为静态声明内存
 * @return: 0 成功，-EINVAL 参数无效
 */
int nos_object_init(struct nos_object *object, bool is_static);

/**
 * 关闭对象 (关闭后禁止接纳新操作与新等待者，具有幂等性)
 *
 * @param object: 目标对象指针
 * @return: 0 成功，-EINVAL 参数无效
 */
int nos_object_close(struct nos_object *object);

/**
 * 查询对象是否已被关闭
 *
 * @param object: 目标对象指针
 * @return: true 已关闭，false 未关闭或对象为空
 */
bool nos_object_is_closed(const struct nos_object *object);

/**
 * 退休对象 (必须在 closed 后调用，释放创建者的持有引用)
 *
 * @param object: 目标对象指针
 * @return: 0 成功，-EINVAL 参数无效或重复 retire，-EBUSY 对象未关闭
 */
int nos_object_retire(struct nos_object *object);

/**
 * 设置对象释放回调
 *
 * @param object: 目标对象指针
 * @param release: 释放回调函数
 */
void nos_object_set_release(struct nos_object *object, nos_object_release_t release);

/**
 * 增加对象原子引用计数
 *
 * @param object: 目标对象指针
 * @return: true 成功获取引用，false 对象已归零或无效
 */
static inline bool nos_object_get(struct nos_object *object)
{
    if (object == NULL) {
        return false;
    }
    return nos_refcount_get(&object->refcount);
}

/**
 * 释放对象原子引用计数并在归零时触发释放回调
 *
 * @param object: 目标对象指针
 * @return: true 引用已归零，false 仍有在途引用或对象无效
 */
static inline bool nos_object_put(struct nos_object *object)
{
    if (object == NULL) {
        return false;
    }

    if (nos_refcount_put(&object->refcount)) {
        if (object->release != NULL) {
            object->release(object);
        }
        return true;
    }

    return false;
}

#endif /* __NOS_OBJECT_H__ */
