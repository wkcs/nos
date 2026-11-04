/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * 宿主端测试：验证通用超时转换 (nos_timeout)、原子引用计数 (nos_refcount)
 * 与对象生命周期管理 (nos_object)。
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include <nos/timeout.h>
#include <nos/refcount.h>
#include <nos/object.h>

/* 模拟系统单调时钟 tick */
static uint64_t s_mock_ticks = 1000ULL;

uint64_t cpu_run_ticks(void)
{
    return s_mock_ticks;
}

/* 释放回调计数器 */
static int s_release_call_count = 0;
static struct nos_object *s_last_released_obj = NULL;

static void mock_release_callback(struct nos_object *obj)
{
    s_release_call_count++;
    s_last_released_obj = obj;
}

/**
 * 测试 1: 超时常量与相对 tick 到单调截止时间 (deadline) 转换及到期判断
 */
static void test_timeout_conversion(void)
{
    uint64_t deadline = 0;
    int rc;

    /* 校验超时常量定义 */
    assert(NOS_NO_WAIT == 0U);
    assert(NOS_WAIT_FOREVER == UINT32_MAX);

    /* 空指针参数校验 */
    rc = nos_deadline_from_timeout(NOS_NO_WAIT, NULL);
    assert(rc == -EINVAL);

    /* NOS_NO_WAIT 转换：deadline 即当前时间戳，且立即判定为已到期 */
    s_mock_ticks = 1000ULL;
    rc = nos_deadline_from_timeout(NOS_NO_WAIT, &deadline);
    assert(rc == 0);
    assert(deadline == 1000ULL);
    assert(nos_deadline_is_expired(deadline) == true);

    /* NOS_WAIT_FOREVER 转换：deadline 为 UINT64_MAX，永不到期 */
    rc = nos_deadline_from_timeout(NOS_WAIT_FOREVER, &deadline);
    assert(rc == 0);
    assert(deadline == UINT64_MAX);
    assert(nos_deadline_is_expired(deadline) == false);

    /* 模拟时钟推进，NOS_WAIT_FOREVER 依然不应到期 */
    s_mock_ticks = UINT64_MAX - 1ULL;
    assert(nos_deadline_is_expired(deadline) == false);

    /* 正常相对 tick 转换与到期检查 */
    s_mock_ticks = 2000ULL;
    rc = nos_deadline_from_timeout(100U, &deadline);
    assert(rc == 0);
    assert(deadline == 2100ULL);

    /* 未到期 */
    assert(nos_deadline_is_expired(deadline) == false);
    s_mock_ticks = 2099ULL;
    assert(nos_deadline_is_expired(deadline) == false);

    /* 刚好到期 */
    s_mock_ticks = 2100ULL;
    assert(nos_deadline_is_expired(deadline) == true);

    /* 逾期 */
    s_mock_ticks = 2101ULL;
    assert(nos_deadline_is_expired(deadline) == true);

    /* 算术溢出防护测试 */
    s_mock_ticks = UINT64_MAX - 50ULL;
    rc = nos_deadline_from_timeout(100U, &deadline);
    assert(rc == -EOVERFLOW);

    puts("[PASS] test_timeout_conversion");
}

/**
 * 测试 2: 原子引用计数 nos_refcount 初始化、增加、减少及归零检测
 */
static void test_refcount(void)
{
    struct nos_refcount ref;

    /* 初始化引用计数为 1 */
    nos_refcount_init(&ref, 1U);
    assert(nos_refcount_read(&ref) == 1U);

    /* 获取引用 (get)：1 -> 2 */
    assert(nos_refcount_get(&ref) == true);
    assert(nos_refcount_read(&ref) == 2U);

    /* 获取引用 (get)：2 -> 3 */
    assert(nos_refcount_get(&ref) == true);
    assert(nos_refcount_read(&ref) == 3U);

    /* 释放引用 (put)：3 -> 2，未归零返回 false */
    assert(nos_refcount_put(&ref) == false);
    assert(nos_refcount_read(&ref) == 2U);

    /* 释放引用 (put)：2 -> 1，未归零返回 false */
    assert(nos_refcount_put(&ref) == false);
    assert(nos_refcount_read(&ref) == 1U);

    /* 释放最后一个引用 (put)：1 -> 0，归零检测返回 true */
    assert(nos_refcount_put(&ref) == true);
    assert(nos_refcount_read(&ref) == 0U);

    /* 归零后禁止复活：get 必须返回 false，计数保持为 0 */
    assert(nos_refcount_get(&ref) == false);
    assert(nos_refcount_read(&ref) == 0U);

    /* 归零后防下溢：put 必须返回 false，计数保持为 0 */
    assert(nos_refcount_put(&ref) == false);
    assert(nos_refcount_read(&ref) == 0U);

    /* 上溢保护测试：UINT32_MAX 拒绝继续递增 */
    nos_refcount_init(&ref, UINT32_MAX);
    assert(nos_refcount_read(&ref) == UINT32_MAX);
    assert(nos_refcount_get(&ref) == false);
    assert(nos_refcount_read(&ref) == UINT32_MAX);

    puts("[PASS] test_refcount");
}

/**
 * 测试 3: 对象生命周期 nos_object 初始化、关闭 (幂等) 及退休流程
 */
static void test_object_lifecycle(void)
{
    struct nos_object obj;
    int rc;

    s_release_call_count = 0;
    s_last_released_obj = NULL;

    /* 参数检查 */
    assert(nos_object_init(NULL, true) == -EINVAL);

    /* 正常静态对象初始化 */
    rc = nos_object_init(&obj, true);
    assert(rc == 0);
    assert(nos_object_is_closed(&obj) == false);
    assert(obj.is_static == true);
    assert(nos_refcount_read(&obj.refcount) == 1U);

    /* 未关闭前退休必须返回 -EBUSY */
    assert(nos_object_retire(&obj) == -EBUSY);

    /* 第一次关闭对象 */
    rc = nos_object_close(&obj);
    assert(rc == 0);
    assert(nos_object_is_closed(&obj) == true);

    /* 重复关闭必须保持幂等，返回 0 且状态不变 */
    rc = nos_object_close(&obj);
    assert(rc == 0);
    assert(nos_object_is_closed(&obj) == true);

    /* 设置自定义释放回调 */
    nos_object_set_release(&obj, mock_release_callback);

    /* 模拟系统调用 lookup 增加在途引用：1 -> 2 */
    assert(nos_object_get(&obj) == true);
    assert(nos_refcount_read(&obj.refcount) == 2U);

    /* 对象所有者执行 retire，释放初始 owner 引用：2 -> 1 */
    rc = nos_object_retire(&obj);
    assert(rc == 0);
    assert(nos_refcount_read(&obj.refcount) == 1U);
    /* 此时仍有在途引用，不应调用 release 回调 */
    assert(s_release_call_count == 0);

    /* 重复 retire 必须被拒绝（防重复释放） */
    assert(nos_object_retire(&obj) == -EINVAL);

    /* 在途系统调用完成，释放最后引用：1 -> 0 */
    assert(nos_object_put(&obj) == true);
    assert(nos_refcount_read(&obj.refcount) == 0U);

    /* 归零时必须触发 release 回调 */
    assert(s_release_call_count == 1);
    assert(s_last_released_obj == &obj);

    puts("[PASS] test_object_lifecycle");
}

int main(void)
{
    puts("=== Running IPC Object Foundation Tests ===");
    test_timeout_conversion();
    test_refcount();
    test_object_lifecycle();
    puts("All IPC object tests PASS");
    return 0;
}
