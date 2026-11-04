/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NOS 通用对象生命周期管理实现。
 */

#include <nos/object.h>
#include <stddef.h>
#include <kernel/errno.h>

int nos_object_init(struct nos_object *object, bool is_static)
{
    if (object == NULL) {
        return -EINVAL;
    }

    spin_lock_init(&object->lock);
    nos_refcount_init(&object->refcount, 1U);
    object->closed = false;
    object->is_static = is_static;
    object->retired = false;
    object->release = NULL;

    return 0;
}

int nos_object_close(struct nos_object *object)
{
    if (object == NULL) {
        return -EINVAL;
    }

    spin_lock(&object->lock);

    /* 幂等性：若已关闭则直接返回成功 */
    if (object->closed) {
        spin_unlock(&object->lock);
        return 0;
    }

    object->closed = true;
    spin_unlock(&object->lock);

    return 0;
}

bool nos_object_is_closed(const struct nos_object *object)
{
    if (object == NULL) {
        return false;
    }

    return __atomic_load_n(&object->closed, __ATOMIC_RELAXED);
}

int nos_object_retire(struct nos_object *object)
{
    if (object == NULL) {
        return -EINVAL;
    }

    spin_lock(&object->lock);

    /* 对象未关闭前禁止退休 */
    if (!object->closed) {
        spin_unlock(&object->lock);
        return -EBUSY;
    }

    /* 防止 owner 重复退休释放 */
    if (object->retired) {
        spin_unlock(&object->lock);
        return -EINVAL;
    }

    object->retired = true;
    spin_unlock(&object->lock);

    /* 锁外释放创建者的 owner 引用 */
    (void)nos_object_put(object);

    return 0;
}

void nos_object_set_release(struct nos_object *object, nos_object_release_t release)
{
    if (object != NULL) {
        object->release = release;
    }
}
