/**
 * Copyright (C) 2026 Nick Hu
 *
 * USB 安全机制与边界稳固性主机单元测试：
 * 1. STM32F4 OTG FS 32 位整字 + 尾部 1~3 字节安全读写逻辑与 Canary 校验；
 * 2. STM32F1 16 位 PMA 读写在奇数字节时使用掩码中转的安全读写逻辑与 Canary 校验；
 * 3. USB 2.0 EP0 ZLP 决策状态机（包含各类边界与多包传输）；
 * 4. 控制传输描述符长度 min_t 钳位机制；
 * 5. SET_CONFIGURATION(0) 重置配置与禁用端点的状态转移。
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdarg.h>

#define CONFIG_SYS_TICK_MS 1
#define __maybe_unused __attribute__((unused))
#ifndef min_t
#define min_t(type, a, b) (((type)(a) < (type)(b)) ? (type)(a) : (type)(b))
#endif
static inline void pr_warning(const char *fmt, ...) { (void)fmt; }

#include <kernel/task.h>
#include <nos/msgq.h>
#include <nos/timeout.h>
#include <kernel/mutex.h>
#include <kernel/printk.h>
#include <drivers/usb/usb_common.h>
#include <drivers/usb/usb_device.h>

/* 桩函数实现以支持包含 core.c 和 usbdevice.c */
void *kmalloc(size_t size, unsigned int flags) { (void)flags; return malloc(size); }
void kfree(void *ptr) { free(ptr); }
int pr_log(bool no_tag, enum log_level level, const char *fmt, ...) { (void)no_tag; (void)level; (void)fmt; return 0; }
int nos_msgq_init(struct nos_msgq *q, void *buf, size_t bsz, size_t esz, uint32_t cap)
{
    (void)q; (void)buf; (void)bsz; (void)esz; (void)cap;
    return 0;
}
int nos_msgq_recv(struct nos_msgq *q, void *msg, size_t sz, nos_timeout_t to)
{
    (void)q; (void)msg; (void)sz; (void)to;
    return 0;
}
int nos_msgq_send(struct nos_msgq *q, const void *msg, size_t sz, nos_timeout_t to)
{
    (void)q; (void)msg; (void)sz; (void)to;
    return 0;
}
struct task_struct *task_create(const char *name, void (*entry)(void *parameter), void *parameter,
                                uint8_t priority, uint32_t stack_size, uint32_t tick,
                                void (*clean)(struct task_struct *task))
{
    (void)name; (void)entry; (void)parameter; (void)priority; (void)stack_size; (void)tick; (void)clean;
    return NULL;
}
int task_ready(struct task_struct *task) { (void)task; return 0; }

#undef pr_fmt
/* 包含待测核心代码 */
#include "../../drivers/usb/device/core/core.c"
#undef pr_fmt
#include "../../drivers/usb/device/core/usbdevice.c"

/* ========================================================================= */
/* 辅助结构与 Canary 哨兵测试脚手架                                           */
/* ========================================================================= */

#define CANARY_GUARD_SIZE 16
#define CANARY_PRE_BYTE   0xAA
#define CANARY_POST_BYTE  0x55
#define POISON_BYTE       0xBD
#define TEST_BUF_CAP      128

struct canary_buffer {
    uint8_t pre_canary[CANARY_GUARD_SIZE];
    uint8_t data[TEST_BUF_CAP];
    uint8_t post_canary[CANARY_GUARD_SIZE];
};

static void init_canary_buffer(struct canary_buffer *cb, uint16_t len, uint8_t base_val, uint8_t tail_fill)
{
    memset(cb->pre_canary, CANARY_PRE_BYTE, sizeof(cb->pre_canary));
    for (uint16_t i = 0; i < len; i++) {
        cb->data[i] = (uint8_t)(base_val + i);
    }
    memset(&cb->data[len], tail_fill, TEST_BUF_CAP - len);
    memset(cb->post_canary, CANARY_POST_BYTE, sizeof(cb->post_canary));
}

static void assert_canary_buffer_clean(const struct canary_buffer *cb, uint16_t len, uint8_t expected_tail_fill, const char *msg)
{
    for (size_t i = 0; i < sizeof(cb->pre_canary); i++) {
        if (cb->pre_canary[i] != CANARY_PRE_BYTE) {
            fprintf(stderr, "FAIL [%s]: 前哨兵字节在索引 %zu 处被破坏！期望 0x%02x, 实际 0x%02x\n",
                    msg, i, CANARY_PRE_BYTE, cb->pre_canary[i]);
            assert(false);
        }
    }
    for (size_t i = len; i < TEST_BUF_CAP; i++) {
        if (cb->data[i] != expected_tail_fill) {
            fprintf(stderr, "FAIL [%s]: 缓冲区在有效数据长度 %u 之后的字节 (索引 %zu) 发生越界写！期望 0x%02x, 实际 0x%02x\n",
                    msg, len, i, expected_tail_fill, cb->data[i]);
            assert(false);
        }
    }
    for (size_t i = 0; i < sizeof(cb->post_canary); i++) {
        if (cb->post_canary[i] != CANARY_POST_BYTE) {
            fprintf(stderr, "FAIL [%s]: 后哨兵字节在索引 %zu 处被破坏！期望 0x%02x, 实际 0x%02x\n",
                    msg, i, CANARY_POST_BYTE, cb->post_canary[i]);
            assert(false);
        }
    }
}

/* ========================================================================= */
/* 1. STM32F4 OTG FS 32 位整字 + 尾部 1~3 字节安全读写测试                    */
/* ========================================================================= */

/* 复现 board/nk60-v2/usb/stm32f4xx_ll_usb.c 中的 USB_WritePacket 算法 */
static void f4_sim_write_packet(uint32_t *fifo, const uint8_t *src, uint16_t len, size_t *out_words)
{
    uint32_t words = len / 4U;
    uint32_t rem = len % 4U;
    uint32_t i = 0U;
    size_t count = 0;

    for (i = 0U; i < words; i++, src += 4U) {
        fifo[count++] = *((const uint32_t *)src);
    }
    if (rem > 0U) {
        uint32_t tail_word = 0U;
        memcpy(&tail_word, src, rem);
        fifo[count++] = tail_word;
    }
    if (out_words) {
        *out_words = count;
    }
}

/* 复现 board/nk60-v2/usb/stm32f4xx_ll_usb.c 中的 USB_ReadPacket 算法 */
static void *f4_sim_read_packet(const uint32_t *fifo, uint8_t *dest, uint16_t len)
{
    uint32_t words = len / 4U;
    uint32_t rem = len % 4U;
    uint32_t i = 0U;
    size_t count = 0;

    for (i = 0U; i < words; i++, dest += 4U) {
        *(uint32_t *)dest = fifo[count++];
    }
    if (rem > 0U) {
        uint32_t tail_word = fifo[count++];
        memcpy(dest, &tail_word, rem);
        dest += rem;
    }
    return ((void *)dest);
}

#include "../../board/nk60-v2/usb/usb_rx_fifo.h"

static void test_stm32f4_out_capacity(void)
{
    volatile uint32_t fifo = 0x44332211U;
    uint8_t data[4] = {0xaa, 0xaa, 0xaa, 0xaa};

    assert(nk60_usb_read_rx_fifo(&fifo, NULL, 1U, 64U) == 0U);
    assert(nk60_usb_read_rx_fifo(&fifo, data, 4U, 1U) == 1U);
    assert(data[0] == 0x11U && data[1] == 0xaaU && data[3] == 0xaaU);
    assert(nk60_usb_read_rx_fifo(&fifo, data + 1, 3U, 3U) == 3U);
    assert(data[1] == 0x11U && data[2] == 0x22U && data[3] == 0x33U);
}

static void test_stm32f4_otg_rw_safety(void)
{
    const uint16_t test_lens[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 63, 64};
    size_t n_tests = sizeof(test_lens) / sizeof(test_lens[0]);
    struct canary_buffer src_cb;
    struct canary_buffer dst_cb;
    uint32_t fifo_buf[64];

    printf("[Test 1] 运行 STM32F4 OTG FS 32位整字+尾部1~3字节安全读写测试...\n");

    for (size_t t = 0; t < n_tests; t++) {
        uint16_t len = test_lens[t];
        size_t words_written = 0;

        /* --- 写入 FIFO 测试 --- */
        /* 在有效数据区后面填满 POISON_BYTE (0xBD)，若算法多读了 1~3 字节，高位会泄露 0xBD */
        init_canary_buffer(&src_cb, len, 0x10, POISON_BYTE);
        memset(fifo_buf, 0, sizeof(fifo_buf));

        f4_sim_write_packet(fifo_buf, src_cb.data, len, &words_written);

        /* 验证写入 FIFO 的字数 */
        size_t expected_words = (len + 3U) / 4U;
        assert(words_written == expected_words);

        /* 验证 FIFO 中有效数据逐字节完全匹配 */
        for (uint16_t i = 0; i < len; i++) {
            uint8_t fifo_byte = ((const uint8_t *)fifo_buf)[i];
            assert(fifo_byte == src_cb.data[i]);
        }

        /* 验证尾部字（如果有未填满的 1~3 字节）高位严格为 0，绝无越界读取 Poison 字节 */
        uint32_t rem = len % 4U;
        if (rem > 0U) {
            uint32_t last_word = fifo_buf[words_written - 1];
            for (uint32_t k = rem; k < 4U; k++) {
                uint8_t pad_byte = (uint8_t)((last_word >> (k * 8)) & 0xFF);
                if (pad_byte != 0x00) {
                    fprintf(stderr, "FAIL: F4 FIFO 写入尾部第 %u 字节包含非法数据 0x%02x (越界读)!\n", k, pad_byte);
                    assert(false);
                }
            }
        }

        /* 验证源缓冲区前后哨兵与尾部 Poison 字节未被篡改 */
        assert_canary_buffer_clean(&src_cb, len, POISON_BYTE, "F4 write src canary");

        /* --- 从 FIFO 读取测试 --- */
        /* 准备 FIFO 测试数据 */
        for (size_t w = 0; w < 64; w++) {
            uint8_t b0 = (uint8_t)(w * 4 + 1);
            uint8_t b1 = (uint8_t)(w * 4 + 2);
            uint8_t b2 = (uint8_t)(w * 4 + 3);
            uint8_t b3 = (uint8_t)(w * 4 + 4);
            fifo_buf[w] = (uint32_t)(b0 | (b1 << 8) | (b2 << 16) | (b3 << 24));
        }

        /* 目标缓冲区在 len 之后全部填满 CANARY_POST_BYTE */
        init_canary_buffer(&dst_cb, 0, 0, CANARY_POST_BYTE);
        f4_sim_read_packet(fifo_buf, dst_cb.data, len);

        /* 验证读取出的有效数据 */
        for (uint16_t i = 0; i < len; i++) {
            uint8_t expected_val = ((const uint8_t *)fifo_buf)[i];
            assert(dst_cb.data[i] == expected_val);
        }

        /* 验证目标缓冲区 len 之后的 Canary 保持原样，绝无 32 位整字越界覆盖 */
        assert_canary_buffer_clean(&dst_cb, len, CANARY_POST_BYTE, "F4 read dst canary");
    }

    printf("  -> STM32F4 OTG FS 读写安全验证通过 (覆盖 0..16, 63, 64 字节，零越界读写)!\n");
}

/* ========================================================================= */
/* 2. STM32F1 16 位 PMA 奇数字节安全读写测试                                   */
/* ========================================================================= */

/* 复现 board/qemu-stm32/usb/stm32f1xx_ll_usb.c 中的 USB_WritePMA 算法 */
static void f1_sim_write_pma(uint16_t *pma, const uint8_t *src, uint16_t wNBytes)
{
    uint32_t halfwords = wNBytes >> 1;
    uint32_t index = 0, temp1 = 0, temp2 = 0;
    uint16_t *pdwVal = pma;

    for (index = halfwords; index != 0; index--) {
        temp1 = (uint16_t)*src++;
        temp2 = temp1 | ((uint16_t)*src++ << 8);
        *pdwVal = temp2;
        pdwVal += 2;
    }
    if (wNBytes & 1) {
        *pdwVal = (uint16_t)*src;
    }
}

/* 复现 board/qemu-stm32/usb/stm32f1xx_ll_usb.c 中的 USB_ReadPMA 算法 */
static void f1_sim_read_pma(const uint16_t *pma, uint8_t *dest, uint16_t wNBytes)
{
    uint32_t halfwords = wNBytes >> 1;
    uint32_t index = 0;
    const uint32_t *pdwVal = (const uint32_t *)pma;

    for (index = halfwords; index != 0; index--) {
        uint32_t val = *pdwVal++;
        *dest++ = (uint8_t)(val & 0xFF);
        *dest++ = (uint8_t)((val >> 8) & 0xFF);
    }
    if (wNBytes & 1) {
        *dest = (uint8_t)(*pdwVal & 0xFF);
    }
}

static void test_stm32f1_pma_rw_safety(void)
{
    const uint16_t test_lens[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 63, 64};
    size_t n_tests = sizeof(test_lens) / sizeof(test_lens[0]);
    struct canary_buffer src_cb;
    struct canary_buffer dst_cb;
    uint16_t pma_buf[128];

    printf("[Test 2] 运行 STM32F1 16位 PMA 奇数字节安全读写测试...\n");

    for (size_t t = 0; t < n_tests; t++) {
        uint16_t len = test_lens[t];

        /* --- 写入 PMA 测试 --- */
        init_canary_buffer(&src_cb, len, 0x20, POISON_BYTE);
        memset(pma_buf, 0, sizeof(pma_buf));

        f1_sim_write_pma(pma_buf, src_cb.data, len);

        /* 验证整半字 */
        for (uint16_t h = 0; h < (len >> 1); h++) {
            uint16_t expected_hw = (uint16_t)(src_cb.data[h * 2] | (src_cb.data[h * 2 + 1] << 8));
            assert(pma_buf[h * 2] == expected_hw);
        }

        /* 验证奇数字节尾部：高 8 位严格为 0，绝无越界读取 Poison 字节 */
        if (len & 1) {
            uint16_t last_hw = pma_buf[(len >> 1) * 2];
            uint8_t low_byte = (uint8_t)(last_hw & 0xFF);
            uint8_t high_byte = (uint8_t)((last_hw >> 8) & 0xFF);
            assert(low_byte == src_cb.data[len - 1]);
            if (high_byte != 0x00) {
                fprintf(stderr, "FAIL: F1 PMA 写入奇数字节高位包含非法数据 0x%02x (越界读)!\n", high_byte);
                assert(false);
            }
        }

        /* 验证源缓冲区未受破坏 */
        assert_canary_buffer_clean(&src_cb, len, POISON_BYTE, "F1 PMA write src canary");

        /* --- 从 PMA 读取测试 --- */
        /* 准备 PMA 数据 */
        for (size_t h = 0; h < 64; h++) {
            uint8_t b0 = (uint8_t)(h * 2 + 1);
            uint8_t b1 = (uint8_t)(h * 2 + 2);
            pma_buf[h * 2] = (uint16_t)(b0 | (b1 << 8));
            pma_buf[h * 2 + 1] = 0;
        }

        init_canary_buffer(&dst_cb, 0, 0, CANARY_POST_BYTE);
        f1_sim_read_pma(pma_buf, dst_cb.data, len);

        /* 验证有效读取字节 */
        for (uint16_t i = 0; i < len; i++) {
            uint16_t hw = pma_buf[(i >> 1) * 2];
            uint8_t expected_val = (i & 1) ? (uint8_t)((hw >> 8) & 0xFF) : (uint8_t)(hw & 0xFF);
            assert(dst_cb.data[i] == expected_val);
        }

        /* 验证目标缓冲区哨兵完整性，尤其奇数长度时 len 索引位置未被破坏 */
        assert_canary_buffer_clean(&dst_cb, len, CANARY_POST_BYTE, "F1 PMA read dst canary");
    }

    printf("  -> STM32F1 PMA 奇数字节安全读写验证通过 (奇偶长度覆盖，零越界读写)!\n");
}

/* ========================================================================= */
/* 3. USB 2.0 EP0 ZLP 决策状态机验证                                          */
/* ========================================================================= */

static struct {
    int write_calls;
    uint8_t write_addr;
    const void *write_buf;
    size_t write_len;

    int read_prep_calls;
    uint8_t read_prep_addr;
    void *read_prep_buf;
    size_t read_prep_len;

    int ep0_status_calls;
    int stall_calls;
    int ep_enable_calls;
    int ep_disable_calls;
    int set_config_val;
} mock_dcd_stats;

static void (*mock_write_observer)(uint8_t address, const void *buffer, size_t size);

static size_t mock_ep_write(uint8_t address, void *buffer, size_t size)
{
    mock_dcd_stats.write_calls++;
    mock_dcd_stats.write_addr = address;
    mock_dcd_stats.write_buf = buffer;
    mock_dcd_stats.write_len = size;
    if (mock_write_observer != NULL) {
        mock_write_observer(address, buffer, size);
    }
    return size;
}

static size_t mock_ep_read_prepare(uint8_t address, void *buffer, size_t size)
{
    mock_dcd_stats.read_prep_calls++;
    mock_dcd_stats.read_prep_addr = address;
    mock_dcd_stats.read_prep_buf = buffer;
    mock_dcd_stats.read_prep_len = size;
    return size;
}

static int mock_ep0_send_status(void)
{
    mock_dcd_stats.ep0_status_calls++;
    return 0;
}

static int mock_ep_set_stall(uint8_t address)
{
    (void)address;
    mock_dcd_stats.stall_calls++;
    return 0;
}

static int mock_ep_clear_stall(uint8_t address)
{
    (void)address;
    return 0;
}

static int mock_ep_enable(struct uendpoint *ep)
{
    (void)ep;
    mock_dcd_stats.ep_enable_calls++;
    return 0;
}

static int mock_ep_disable(struct uendpoint *ep)
{
    (void)ep;
    mock_dcd_stats.ep_disable_calls++;
    return 0;
}

static int mock_set_config(uint8_t address)
{
    mock_dcd_stats.set_config_val = address;
    return 0;
}

static const struct udcd_ops mock_udcd_ops = {
    .ep_write = mock_ep_write,
    .ep_read_prepare = mock_ep_read_prepare,
    .ep0_send_status = mock_ep0_send_status,
    .ep_set_stall = mock_ep_set_stall,
    .ep_clear_stall = mock_ep_clear_stall,
    .ep_enable = mock_ep_enable,
    .ep_disable = mock_ep_disable,
    .set_config = mock_set_config,
};

static void reset_mock_dcd_stats(void)
{
    memset(&mock_dcd_stats, 0, sizeof(mock_dcd_stats));
    mock_dcd_stats.set_config_val = -1;
}

static void test_ep0_zlp_state_machine(void)
{
    struct udcd dcd;
    struct ep_id ep0_id;
    uint8_t test_data[512];

    for (size_t i = 0; i < sizeof(test_data); i++) {
        test_data[i] = (uint8_t)(i & 0xFF);
    }

    printf("[Test 3] 运行 USB 2.0 EP0 ZLP 决策状态机全分支测试...\n");

    /*
     * 场景 1: total_sent == req_wlength 且为 MPS 整数倍 (例如 sent=64, req=64, MPS=64)
     * 根据 USB 2.0 规范 8.5.3.2 节：当发送长度等于主机请求长度时，绝对不能发 ZLP，
     * 直接转入 STATUS 阶段！
     */
    {
        reset_mock_dcd_stats();
        memset(&dcd, 0, sizeof(dcd));
        memset(&ep0_id, 0, sizeof(ep0_id));
        ep0_id.maxpacket = 64;
        dcd.ops = &mock_udcd_ops;
        dcd.ep0.id = &ep0_id;
        dcd.stage = STAGE_DIN;
        dcd.ep0.request.req_wlength = 64;
        dcd.ep0.request.size = 64;
        dcd.ep0.request.remain_size = 64;
        dcd.ep0.request.buffer = test_data;

        int ret = usbd_ep0_in_handler(&dcd);
        assert(ret == 0);
        assert(mock_dcd_stats.write_calls == 0 && "total_sent == req_wlength 不应发送 ZLP！");
        assert(dcd.stage == STAGE_STATUS_OUT && "必须进入 STAGE_STATUS_OUT 阶段！");
        assert(mock_dcd_stats.read_prep_calls == 1);
        assert(mock_dcd_stats.read_prep_len == 0);
    }

    /*
     * 场景 2: total_sent < req_wlength 且为 MPS 整数倍 (例如 sent=64, req=128, MPS=64)
     * 根据规范：数据已发完且是 MPS 整数倍但严格小于主机期待长度，必须发送 ZLP 告知主机结束！
     */
    {
        reset_mock_dcd_stats();
        memset(&dcd, 0, sizeof(dcd));
        memset(&ep0_id, 0, sizeof(ep0_id));
        ep0_id.maxpacket = 64;
        dcd.ops = &mock_udcd_ops;
        dcd.ep0.id = &ep0_id;
        dcd.stage = STAGE_DIN;
        dcd.ep0.request.req_wlength = 128;
        dcd.ep0.request.size = 64;
        dcd.ep0.request.remain_size = 64;
        dcd.ep0.request.buffer = test_data;

        int ret = usbd_ep0_in_handler(&dcd);
        assert(ret == 0);
        assert(mock_dcd_stats.write_calls == 1 && "必须触发 ZLP 写入！");
        assert(mock_dcd_stats.write_len == 0 && "ZLP 写入长度必须为 0！");
        assert(mock_dcd_stats.write_addr == EP0_IN_ADDR);
        assert(dcd.ep0.request.req_wlength == 0);

        /* 随后主机对 ZLP 发起 IN token 确认，再次调用 usbd_ep0_in_handler */
        ret = usbd_ep0_in_handler(&dcd);
        assert(ret == 0);
        assert(dcd.stage == STAGE_STATUS_OUT && "ZLP 发送完成后必须转入 STAGE_STATUS_OUT！");
        assert(mock_dcd_stats.read_prep_calls == 1);
    }

    /*
     * 场景 3: total_sent < req_wlength 且为短包 (小于 MPS，例如 sent=18, req=64, MPS=64)
     * 短包本身已经隐式终结传输，绝对不应发送额外的 ZLP！
     */
    {
        reset_mock_dcd_stats();
        memset(&dcd, 0, sizeof(dcd));
        memset(&ep0_id, 0, sizeof(ep0_id));
        ep0_id.maxpacket = 64;
        dcd.ops = &mock_udcd_ops;
        dcd.ep0.id = &ep0_id;
        dcd.stage = STAGE_DIN;
        dcd.ep0.request.req_wlength = 64;
        dcd.ep0.request.size = 18;
        dcd.ep0.request.remain_size = 18;
        dcd.ep0.request.buffer = test_data;

        int ret = usbd_ep0_in_handler(&dcd);
        assert(ret == 0);
        assert(mock_dcd_stats.write_calls == 0 && "短包自动终结传输，绝不能发送 ZLP！");
        assert(dcd.stage == STAGE_STATUS_OUT);
        assert(mock_dcd_stats.read_prep_calls == 1);
    }

    /*
     * 场景 4: total_sent == 0 (零长度数据发送)
     * 绝不能发送 ZLP！
     */
    {
        reset_mock_dcd_stats();
        memset(&dcd, 0, sizeof(dcd));
        memset(&ep0_id, 0, sizeof(ep0_id));
        ep0_id.maxpacket = 64;
        dcd.ops = &mock_udcd_ops;
        dcd.ep0.id = &ep0_id;
        dcd.stage = STAGE_DIN;
        dcd.ep0.request.req_wlength = 0;
        dcd.ep0.request.size = 0;
        dcd.ep0.request.remain_size = 0;
        dcd.ep0.request.buffer = test_data;

        int ret = usbd_ep0_in_handler(&dcd);
        assert(ret == 0);
        assert(mock_dcd_stats.write_calls == 0 && "total_sent == 0 绝不能发送 ZLP！");
        assert(dcd.stage == STAGE_STATUS_OUT);
        assert(mock_dcd_stats.read_prep_calls == 1);
    }

    /*
     * 场景 5: 多包传输带 ZLP (sent=128, req=256, MPS=64)
     * Packet 1 发完 -> 还有 64 字节未完 -> 继续发 Packet 2；
     * Packet 2 发完 -> 达到 128 字节 (MPS 整数倍，< 256) -> 必须发 ZLP；
     * ZLP 确认 -> 进入 STATUS 阶段。
     */
    {
        reset_mock_dcd_stats();
        memset(&dcd, 0, sizeof(dcd));
        memset(&ep0_id, 0, sizeof(ep0_id));
        ep0_id.maxpacket = 64;
        dcd.ops = &mock_udcd_ops;
        dcd.ep0.id = &ep0_id;
        dcd.stage = STAGE_DIN;
        dcd.ep0.request.req_wlength = 256;
        dcd.ep0.request.size = 128;
        dcd.ep0.request.remain_size = 128;
        dcd.ep0.request.buffer = test_data;

        /* 第一包完成中断 */
        int ret = usbd_ep0_in_handler(&dcd);
        assert(ret == 0);
        assert(mock_dcd_stats.write_calls == 1 && "应发出第二包数据！");
        assert(mock_dcd_stats.write_len == 64);
        assert(dcd.stage == STAGE_DIN);

        /* 第二包完成中断 */
        ret = usbd_ep0_in_handler(&dcd);
        assert(ret == 0);
        assert(mock_dcd_stats.write_calls == 2 && "第二包发完后必须发送 ZLP！");
        assert(mock_dcd_stats.write_len == 0);

        /* ZLP 完成中断 */
        ret = usbd_ep0_in_handler(&dcd);
        assert(ret == 0);
        assert(dcd.stage == STAGE_STATUS_OUT);
        assert(mock_dcd_stats.read_prep_calls == 1);
    }

    /*
     * 场景 6: 多包传输无 ZLP (sent=128, req=128, MPS=64)
     * 第二包正好发到 128 且等于 req_wlength，绝不发 ZLP！
     */
    {
        reset_mock_dcd_stats();
        memset(&dcd, 0, sizeof(dcd));
        memset(&ep0_id, 0, sizeof(ep0_id));
        ep0_id.maxpacket = 64;
        dcd.ops = &mock_udcd_ops;
        dcd.ep0.id = &ep0_id;
        dcd.stage = STAGE_DIN;
        dcd.ep0.request.req_wlength = 128;
        dcd.ep0.request.size = 128;
        dcd.ep0.request.remain_size = 128;
        dcd.ep0.request.buffer = test_data;

        /* 第一包完成 */
        int ret = usbd_ep0_in_handler(&dcd);
        assert(ret == 0);
        assert(mock_dcd_stats.write_calls == 1 && mock_dcd_stats.write_len == 64);

        /* 第二包完成 */
        ret = usbd_ep0_in_handler(&dcd);
        assert(ret == 0);
        assert(mock_dcd_stats.write_calls == 1 && "发满 req_wlength 绝不能发送 ZLP！");
        assert(dcd.stage == STAGE_STATUS_OUT);
        assert(mock_dcd_stats.read_prep_calls == 1);
    }

    /*
     * 场景 7: 多包传输尾包为短包 (sent=70, req=128, MPS=64)
     * 第一包 64 字节，第二包 6 字节；短包自动终结传输，绝无 ZLP！
     */
    {
        reset_mock_dcd_stats();
        memset(&dcd, 0, sizeof(dcd));
        memset(&ep0_id, 0, sizeof(ep0_id));
        ep0_id.maxpacket = 64;
        dcd.ops = &mock_udcd_ops;
        dcd.ep0.id = &ep0_id;
        dcd.stage = STAGE_DIN;
        dcd.ep0.request.req_wlength = 128;
        dcd.ep0.request.size = 70;
        dcd.ep0.request.remain_size = 70;
        dcd.ep0.request.buffer = test_data;

        /* 第一包完成 */
        int ret = usbd_ep0_in_handler(&dcd);
        assert(ret == 0);
        assert(mock_dcd_stats.write_calls == 1 && mock_dcd_stats.write_len == 6);

        /* 第二包（短包）完成 */
        ret = usbd_ep0_in_handler(&dcd);
        assert(ret == 0);
        assert(mock_dcd_stats.write_calls == 1 && "尾部短包已终结传输，绝无 ZLP！");
        assert(dcd.stage == STAGE_STATUS_OUT);
        assert(mock_dcd_stats.read_prep_calls == 1);
    }

    /*
     * 场景 8: 多种 MPS 规格 (MPS = 8, 16, 32)
     */
    uint16_t mps_list[] = {8, 16, 32};
    for (size_t m = 0; m < sizeof(mps_list)/sizeof(mps_list[0]); m++) {
        uint16_t mps = mps_list[m];
        reset_mock_dcd_stats();
        memset(&dcd, 0, sizeof(dcd));
        memset(&ep0_id, 0, sizeof(ep0_id));
        ep0_id.maxpacket = mps;
        dcd.ops = &mock_udcd_ops;
        dcd.ep0.id = &ep0_id;
        dcd.stage = STAGE_DIN;
        dcd.ep0.request.req_wlength = mps * 2;
        dcd.ep0.request.size = mps;
        dcd.ep0.request.remain_size = mps;
        dcd.ep0.request.buffer = test_data;

        int ret = usbd_ep0_in_handler(&dcd);
        assert(ret == 0);
        assert(mock_dcd_stats.write_calls == 1 && mock_dcd_stats.write_len == 0 && "MPS 整数倍且小于 req_wlength 必须发 ZLP！");
    }

    printf("  -> USB 2.0 EP0 ZLP 决策状态机验证通过 (7大场景+多种MPS边界全覆盖)!\n");
}

/* ========================================================================= */
/* 4. 描述符与控制传输长度截断与 min_t 钳位测试                                */
/* ========================================================================= */

static void test_control_transfer_length_clamp(void)
{
    printf("[Test 4] 运行描述符与控制传输长度截断与 min_t 钳位测试...\n");

    /* 4.1 min_t 宏类型安全性与求值测试 */
    assert(min_t(size_t, 10, 20) == 10);
    assert(min_t(size_t, 20, 10) == 10);
    assert(min_t(size_t, 0, 50) == 0);
    assert(min_t(uint16_t, 0xFFFF, 64) == 64);
    assert(min_t(int32_t, -5, 10) == -5);

    /* 4.2 设备描述符长度钳位测试 */
    {
        struct udevice dev = {0};
        struct udcd dcd = {0};
        struct ep_id ep0_id = {.maxpacket = 64, .addr = 0, .dir = USB_DIR_IN};
        struct urequest setup = {0};

        dcd.ops = &mock_udcd_ops;
        dcd.ep0.id = &ep0_id;
        dev.dcd = &dcd;

        /* 请求 8 字节（主机初次枚举获取 maxpacket） */
        reset_mock_dcd_stats();
        setup.wLength = 8;
        _get_device_descriptor(&dev, &setup);
        assert(mock_dcd_stats.write_calls == 1);
        assert(mock_dcd_stats.write_len == 8);

        /* 请求 18 字节（标准设备描述符全长） */
        reset_mock_dcd_stats();
        setup.wLength = USB_DESC_LENGTH_DEVICE;
        _get_device_descriptor(&dev, &setup);
        assert(mock_dcd_stats.write_calls == 1);
        assert(mock_dcd_stats.write_len == USB_DESC_LENGTH_DEVICE);

        /* 请求 255 字节（超出标准长度，必须钳制至 18 字节） */
        reset_mock_dcd_stats();
        setup.wLength = 255;
        _get_device_descriptor(&dev, &setup);
        assert(mock_dcd_stats.write_calls == 1);
        assert(mock_dcd_stats.write_len == USB_DESC_LENGTH_DEVICE);

        /* 请求 0 字节 */
        reset_mock_dcd_stats();
        setup.wLength = 0;
        _get_device_descriptor(&dev, &setup);
        assert(mock_dcd_stats.write_calls == 1);
        assert(mock_dcd_stats.write_len == 0);
    }

    /* 4.3 配置描述符长度钳位测试 */
    {
        struct udevice dev = {0};
        struct udcd dcd = {0};
        struct ep_id ep0_id = {.maxpacket = 64, .addr = 0, .dir = USB_DIR_IN};
        struct uconfig cfg = {0};
        struct urequest setup = {0};

        dcd.ops = &mock_udcd_ops;
        dcd.ep0.id = &ep0_id;
        dev.dcd = &dcd;
        dev.curr_cfg = &cfg;
        cfg.cfg_desc.wTotalLength = 32;

        /* 请求 9 字节（配置描述符头部） */
        reset_mock_dcd_stats();
        setup.wLength = 9;
        _get_config_descriptor(&dev, &setup);
        assert(mock_dcd_stats.write_len == 9);

        /* 请求全长 32 字节 */
        reset_mock_dcd_stats();
        setup.wLength = 32;
        _get_config_descriptor(&dev, &setup);
        assert(mock_dcd_stats.write_len == 32);

        /* 主机请求 256 字节（大于实际总长 32），必须钳位至 32 */
        reset_mock_dcd_stats();
        setup.wLength = 256;
        _get_config_descriptor(&dev, &setup);
        assert(mock_dcd_stats.write_len == 32);

        /* 极大长度 0xFFFF，必须钳位至 32 且不超过 sizeof(cfg.raw_desc) */
        reset_mock_dcd_stats();
        setup.wLength = 0xFFFF;
        _get_config_descriptor(&dev, &setup);
        assert(mock_dcd_stats.write_len == 32);
    }

    /* 4.4 字符串描述符长度钳位与缓冲区保护测试 */
    {
        struct udevice dev = {0};
        struct udcd dcd = {0};
        struct ep_id ep0_id = {.maxpacket = 64, .addr = 0, .dir = USB_DIR_IN};
        struct urequest setup = {0};
        const char *test_strs[USB_STRING_MAX] = {0};

        dcd.ops = &mock_udcd_ops;
        dcd.ep0.id = &ep0_id;
        dev.dcd = &dcd;
        dev.str = test_strs;

        /* LANGID 描述符 (固定 4 字节) */
        test_strs[USB_STRING_LANGID_INDEX] = "LANGID";
        reset_mock_dcd_stats();
        setup.wValue = USB_STRING_LANGID_INDEX;
        setup.wLength = 255;
        _get_string_descriptor(&dev, &setup);
        assert(mock_dcd_stats.write_len == 4);

        /* 正常字符串 "NOS" (3 字符 -> 2 + 3*2 = 8 字节) */
        test_strs[1] = "NOS";
        reset_mock_dcd_stats();
        setup.wValue = 1;
        setup.wLength = 255;
        _get_string_descriptor(&dev, &setup);
        assert(mock_dcd_stats.write_len == 8);

        /* 主机只要 2 字节（获取 bLength） */
        reset_mock_dcd_stats();
        setup.wValue = 1;
        setup.wLength = 2;
        _get_string_descriptor(&dev, &setup);
        assert(mock_dcd_stats.write_len == 2);

        /* 超长字符串 (100 字符)，必须被钳制至 (sizeof(str_desc_buf) - 2)/2 字符 = 63 字符 */
        char overlong_str[101];
        memset(overlong_str, 'A', 100);
        overlong_str[100] = '\0';
        test_strs[2] = overlong_str;

        reset_mock_dcd_stats();
        setup.wValue = 2;
        setup.wLength = 255;
        _get_string_descriptor(&dev, &setup);
        /* 最大描述符总长应为 63*2 + 2 = 128 字节 */
        assert(dev.str_desc_buf[0] == 128);
        assert(dcd.ep0.request.size == 128);
        assert(mock_dcd_stats.write_len == 64);
    }

    /* 4.5 OS Property 描述符 min_t 长度钳位测试 */
    {
        struct udevice dev = {0};
        struct udcd dcd = {0};
        struct ep_id ep0_id = {.maxpacket = 64, .addr = 0, .dir = USB_DIR_IN};
        struct ufunction func = {0};
        struct urequest setup = {0};
        struct usb_os_property_header os_header;

        dcd.ops = &mock_udcd_ops;
        dcd.ep0.id = &ep0_id;
        dev.dcd = &dcd;
        func.device = &dev;
        os_header.dwLength = 48;

        /* 首次调用动态构建 OS Property Descriptor (无子属性时头部为 10 字节) */
        /* 当主机请求 wLength = 100 > actual_len(10) 时，钳制至 10 */
        reset_mock_dcd_stats();
        setup.wLength = 100;
        usbd_os_proerty_descriptor_send(&func, &setup, (usb_os_proerty_t)&os_header, 0);
        assert(mock_dcd_stats.write_len == 10);

        /* 当主机请求 wLength = 4 < actual_len(10) 时，钳制至 4 */
        reset_mock_dcd_stats();
        setup.wLength = 4;
        usbd_os_proerty_descriptor_send(&func, &setup, (usb_os_proerty_t)&os_header, 0);
        assert(mock_dcd_stats.write_len == 4);
    }

    printf("  -> 控制传输描述符 min_t 长度钳位验证通过 (设备/配置/字符串/OS描述符全覆盖)!\n");
}

/* ========================================================================= */
/* 5. SET_CONFIGURATION(0) 状态转移与端点清理测试                              */
/* ========================================================================= */

static int mock_func_disable_called = 0;
static int mock_func_disable(struct ufunction *func)
{
    (void)func;
    mock_func_disable_called++;
    return 0;
}

static struct ufunction_ops mock_func_ops = {
    .disable = mock_func_disable,
};

static void test_set_configuration_transition(void)
{
    struct udevice dev = {0};
    struct udcd dcd = {0};
    struct ep_id ep0_id = {.maxpacket = 64, .addr = 0, .dir = USB_DIR_IN};
    struct uconfig cfg = {0};
    struct ufunction func = {0};
    struct uinterface intf = {0};
    struct ualtsetting setting = {0};
    struct uendpoint ep1 = {0};
    struct uendpoint ep2 = {0};
    struct urequest setup = {0};

    printf("[Test 5] 运行 SET_CONFIGURATION(0) 重置配置与端点禁用状态转移测试...\n");

    /* 初始化设备与驱动链表层次 */
    dcd.ops = &mock_udcd_ops;
    dcd.ep0.id = &ep0_id;
    dev.dcd = &dcd;
    dev.state = USB_STATE_CONFIGURED;
    dev.dev_desc.bNumConfigurations = 1;

    INIT_LIST_HEAD(&dev.cfg_list);
    INIT_LIST_HEAD(&cfg.func_list);
    INIT_LIST_HEAD(&func.intf_list);
    INIT_LIST_HEAD(&intf.setting_list);
    INIT_LIST_HEAD(&setting.ep_list);

    list_add_tail(&cfg.list, &dev.cfg_list);
    list_add_tail(&func.list, &cfg.func_list);
    list_add_tail(&intf.list, &func.intf_list);
    list_add_tail(&ep1.list, &setting.ep_list);
    list_add_tail(&ep2.list, &setting.ep_list);

    intf.curr_setting = &setting;
    func.ops = &mock_func_ops;
    func.enabled = true;
    dev.curr_cfg = &cfg;

    /* 执行 SET_CONFIGURATION(0) */
    reset_mock_dcd_stats();
    mock_func_disable_called = 0;
    setup.wValue = 0;

    int ret = _set_config(&dev, &setup);
    assert(ret == 0);

    /* 断言 1: 配置被清空 */
    assert(dev.curr_cfg == NULL && "SET_CONFIGURATION(0) 必须清空 curr_cfg！");

    /* 断言 2: 设备状态转移回 ADDRESS 状态 */
    assert(dev.state == USB_STATE_ADDRESS && "SET_CONFIGURATION(0) 必须切回 USB_STATE_ADDRESS！");

    /* 断言 3: 底层 dcd_set_config(0) 被调用 */
    assert(mock_dcd_stats.set_config_val == 0 && "底层 dcd_set_config(0) 必须被调用！");

    /* 断言 4: 该配置下的 2 个端点均被调用 dcd_ep_disable */
    assert(mock_dcd_stats.ep_disable_calls == 2 && "配置下的所有端点必须被 disable！");

    /* 断言 5: 功能驱动的 disable 回调被触发 */
    assert(mock_func_disable_called == 1 && "功能驱动的 disable 回调必须被调用！");

    /* 断言 6: 状态阶段被响应 */
    assert(mock_dcd_stats.ep0_status_calls == 1 && "必须发出 EP0 状态阶段包！");

    /* 测试非法配置号 (wValue > bNumConfigurations) */
    reset_mock_dcd_stats();
    setup.wValue = 99;
    ret = _set_config(&dev, &setup);
    assert(ret == -1 && "非法配置号必须返回 -1！");
    assert(mock_dcd_stats.stall_calls == 1 && "非法配置号必须 STALL EP0！");

    printf("  -> SET_CONFIGURATION(0) 状态转移与端点清理验证通过!\n");
}

/* ========================================================================= */
/* A fast IN completion can wake the higher-priority USB task before the
 * submitting task returns from the DCD write. Exercise that exact boundary. */
static struct udevice *preempt_write_device;
static bool preempt_first_write;
static uint8_t preempt_wire_data[1024];
static size_t preempt_wire_size;
static unsigned int preempt_completions;

static int preempt_in_complete(struct ufunction *func, size_t size)
{
    (void)func;
    (void)size;
    preempt_completions++;
    return 0;
}

static void preempt_write_observer(uint8_t address, const void *buffer, size_t size)
{
    assert(preempt_wire_size + size <= sizeof(preempt_wire_data));
    if (size > 0U) {
        memcpy(preempt_wire_data + preempt_wire_size, buffer, size);
    }
    preempt_wire_size += size;
    if (preempt_first_write) {
        struct ep_msg event = {.ep_addr = address, .size = size};

        preempt_first_write = false;
        assert(_data_notify(preempt_write_device, &event) == 0);
    }
}

static void test_in_completion_preempts_submitter(void)
{
    const size_t lengths[] = {0U, 1U, 58U, 64U, 65U, 128U, 189U, 268U};
    struct udevice dev = {0};
    struct udcd dcd = {.ops = &mock_udcd_ops};
    struct uconfig cfg = {0};
    struct ufunction func = {0};
    struct uinterface intf = {0};
    struct ualtsetting setting = {0};
    struct uendpoint_descriptor desc = {.bEndpointAddress = 0x81, .wMaxPacketSize = 64};
    struct uendpoint ep = {.ep_desc = &desc, .handler = preempt_in_complete};
    uint8_t payload[268];
    size_t i;
    size_t j;

    puts("[Test 6] IN completion preempts the submitting task...");
    dev.dcd = &dcd;
    dev.curr_cfg = &cfg;
    dev.state = USB_STATE_CONFIGURED;
    func.device = &dev;
    intf.curr_setting = &setting;
    INIT_LIST_HEAD(&cfg.func_list);
    INIT_LIST_HEAD(&func.intf_list);
    INIT_LIST_HEAD(&setting.ep_list);
    list_add_tail(&func.list, &cfg.func_list);
    list_add_tail(&intf.list, &func.intf_list);
    list_add_tail(&ep.list, &setting.ep_list);
    for (i = 0U; i < sizeof(payload); i++) {
        payload[i] = (uint8_t)(i ^ (i >> 6U));
    }

    for (i = 0U; i < sizeof(lengths) / sizeof(lengths[0]); i++) {
        struct ep_msg event = {.ep_addr = 0x81, .size = 64};

        ep.request.buffer = lengths[i] > 0U ? payload : NULL;
        ep.request.size = lengths[i];
        ep.request.req_type = UIO_REQUEST_WRITE;
        preempt_write_device = &dev;
        preempt_first_write = true;
        preempt_wire_size = 0U;
        preempt_completions = 0U;
        mock_write_observer = preempt_write_observer;
        assert(usbd_io_request(&dev, &ep, &ep.request) == lengths[i]);
        for (j = 0U; preempt_completions == 0U && j < 16U; j++) {
            assert(_data_notify(&dev, &event) == 0);
        }
        mock_write_observer = NULL;
        assert(preempt_completions == 1U);
        assert(preempt_wire_size == lengths[i] && "IN packet was duplicated");
        assert(memcmp(preempt_wire_data, payload, lengths[i]) == 0);
        assert(ep.request.remain_size == 0U);
    }
}

/* 主函数                                                                    */
/* ========================================================================= */

int main(void)
{
    printf("================================================================\n");
    printf("开始执行 USB 协议栈核心安全机制与边界稳固性单元测试\n");
    printf("================================================================\n");

    test_stm32f4_otg_rw_safety();
    test_stm32f4_out_capacity();
    test_stm32f1_pma_rw_safety();
    test_ep0_zlp_state_machine();
    test_control_transfer_length_clamp();
    test_set_configuration_transition();
    test_in_completion_preempts_submitter();

    printf("================================================================\n");
    printf("所有 USB 安全与状态机单元测试全部通过 (ALL PASSED)!\n");
    printf("================================================================\n");

    return 0;
}
