/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 Generic Timer 128-bit 防溢出换算、IRQ 分发表与 AI UAPI ABI 尺寸测试
 */

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <uapi/nos/errno.h>
#include <uapi/nos/syscall.h>
#include <uapi/nos/cap.h>
#include <uapi/nos/ipc.h>
#include <uapi/nos/ai.h>
#include <asm/irq.h>

/* 供主机测试直接验证 timer.c 与 irq.c 的纯算法路径 */
extern uint64_t arm64_timer_ticks_to_ns(uint64_t ticks, uint64_t freq);
extern uint64_t arm64_timer_ns_to_ticks(uint64_t ns, uint64_t freq);
extern void arm64_irq_table_reset(void);
extern int arm64_irq_dispatch_intid(uint32_t intid);

struct test_irq_ctx {
    uint32_t count;
    uint32_t last_intid;
};

static void sample_irq_cb(uint32_t intid, void *ctx)
{
    struct test_irq_ctx *st = (struct test_irq_ctx *)ctx;
    st->count++;
    st->last_intid = intid;
}

static void test_ai_uapi_abi(void)
{
    assert(SYS_NOS_MAX == 24);
    assert(sizeof(struct nos_ipc_msg) == 64U);
    assert(sizeof(struct nos_ring_sqe) == 64U);
    assert(sizeof(struct nos_ring_cqe) == 32U);
    assert(sizeof(struct nos_audit_record) == 64U);
    assert(sizeof(struct nos_cell_budget) == 40U);
    assert(sizeof(struct nos_vmo_create_args) == 32U);
    assert(sizeof(struct nos_infer_sched_param) == 32U);
    assert(NOS_CAP_CELL > NOS_CAP_IOPORT);
    assert(NOS_CAP_SNAPSHOT > NOS_CAP_CELL);
}

static void test_timer_128bit_no_overflow(void)
{
    const uint64_t freq_qemu = 62500000ULL; /* 62.5 MHz */

    assert(arm64_timer_ticks_to_ns(0ULL, freq_qemu) == 0ULL);
    assert(arm64_timer_ticks_to_ns(freq_qemu, freq_qemu) == 1000000000ULL);
    assert(arm64_timer_ticks_to_ns(100ULL, 0ULL) == 0ULL);

    /* 校验超过 64-bit 乘法阈值的长时间（例如 10 天：在旧 64-bit 乘法下 5 分钟就溢出） */
    uint64_t ten_days_sec = 10ULL * 86400ULL;
    uint64_t ticks_10d = freq_qemu * ten_days_sec;
    uint64_t ns_10d = arm64_timer_ticks_to_ns(ticks_10d, freq_qemu);
    assert(ns_10d == ten_days_sec * 1000000000ULL);

    /* 双向换算：1ms 与 100us */
    uint64_t t_1ms = arm64_timer_ns_to_ticks(1000000ULL, freq_qemu);
    assert(t_1ms == 62500ULL);
    assert(arm64_timer_ticks_to_ns(t_1ms, freq_qemu) == 1000000ULL);
}

static void test_irq_dispatch_table(void)
{
    struct test_irq_ctx ctx = { 0U, 0U };
    arm64_irq_table_reset();

    /* 越界与 NULL 参数校验 */
    assert(nos_irq_register(33U, NULL, &ctx) == -EINVAL);
    assert(nos_irq_register(1020U, sample_irq_cb, &ctx) == -EINVAL);

    /* 正常注册、重复保护与触发 */
    assert(nos_irq_register(33U, sample_irq_cb, &ctx) == 0);
    assert(nos_irq_register(33U, sample_irq_cb, &ctx) == -EBUSY);
    assert(arm64_irq_dispatch_intid(33U) == 0);
    assert(ctx.count == 1U && ctx.last_intid == 33U);

    /* 伪中断安全忽略 */
    assert(arm64_irq_dispatch_intid(1023U) == -ENOENT);
    assert(ctx.count == 1U);

    /* 注销后不再触发 */
    assert(nos_irq_unregister(33U) == 0);
    assert(nos_irq_unregister(33U) == -ENOENT);
    assert(arm64_irq_dispatch_intid(33U) == -ENOENT);
    assert(ctx.count == 1U);
}

int main(void)
{
    test_ai_uapi_abi();
    test_timer_128bit_no_overflow();
    test_irq_dispatch_table();
    printf("arm64 gic, timer and ai uapi checks passed\n");
    return 0;
}
