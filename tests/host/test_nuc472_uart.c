/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 UART0 中断驱动、环形缓冲与 Panic 主机单元测试
 */
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unistd.h>
#include <pthread.h>

#define NUC472_HOST_TEST 1
#include <soc/nuc472_regs.h>
#include <soc/nuc472_periph.h>
#include <soc/nuc472_soc.h>
#include <kernel/types.h>
#include <kernel/errno.h>
#include <kernel/irq.h>
#include <kernel/device.h>
#include <nos/sem.h>
#include <drivers/serial.h>

/* 模拟硬件 UART0 寄存器 */
static NUC472_UART_Type g_test_uart;
static NUC472_SYS_Type g_test_sys;
static NUC472_CLK_Type g_test_clk;

NUC472_SYS_Type *nuc472_test_sys(void) { return &g_test_sys; }
NUC472_CLK_Type *nuc472_test_clk(void) { return &g_test_clk; }
NUC472_FMC_Type *nuc472_test_fmc(void) { return NULL; }
NUC472_RTC_Type *nuc472_test_rtc(void) { return NULL; }

/* 硬件模拟 FIFO 缓冲区 */
#define HW_FIFO_CAPACITY 64
static uint8_t g_hw_rx_fifo[HW_FIFO_CAPACITY];
static uint32_t g_hw_rx_head = 0;
static uint32_t g_hw_rx_tail = 0;
static uint32_t g_hw_rx_count = 0;

static uint8_t g_hw_tx_buf[1024];
static uint32_t g_hw_tx_count = 0;

/* 模拟中断与上下文桩 */
static bool g_in_irq = false;
static addr_t g_irq_flags = 0;

addr_t disable_irq_save(void)
{
    addr_t prev = g_irq_flags;
    g_irq_flags = 1;
    return prev;
}

void enable_irq_save(addr_t flags)
{
    g_irq_flags = flags;
}

bool hal_in_irq(void)
{
    return g_in_irq;
}

bool in_interrupt(void)
{
    return g_in_irq;
}

static uint32_t g_test_uart0_clk_hz = 12000000UL;

uint32_t nuc472_clk_get_uart0_hz(void)
{
    return g_test_uart0_clk_hz;
}

void nuc472_mfp_set(volatile uint32_t *reg, uint32_t mask, uint32_t val)
{
    if (reg != NULL) {
        *reg = (*reg & ~mask) | (val & mask);
    }
}

/* 硬件模拟数据端口钩子 */
uint8_t host_test_uart_read_dat(void)
{
    if (g_hw_rx_count == 0) {
        g_test_uart.FIFOSTS |= NUC472_UART_FIFOSTS_RXEMPTY;
        return 0;
    }
    uint8_t byte = g_hw_rx_fifo[g_hw_rx_tail];
    g_hw_rx_tail = (g_hw_rx_tail + 1) % HW_FIFO_CAPACITY;
    g_hw_rx_count--;
    if (g_hw_rx_count == 0) {
        g_test_uart.FIFOSTS |= NUC472_UART_FIFOSTS_RXEMPTY;
    }
    return byte;
}

void host_test_uart_write_dat(uint8_t val)
{
    if (g_hw_tx_count < sizeof(g_hw_tx_buf)) {
        g_hw_tx_buf[g_hw_tx_count++] = val;
    }
    g_test_uart.DAT = val;
}

void host_test_uart_clear_fifosts(uint32_t flags)
{
    g_test_uart.FIFOSTS &= ~flags;
}

static void hw_rx_inject(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        assert(g_hw_rx_count < HW_FIFO_CAPACITY);
        g_hw_rx_fifo[g_hw_rx_head] = data[i];
        g_hw_rx_head = (g_hw_rx_head + 1) % HW_FIFO_CAPACITY;
        g_hw_rx_count++;
    }
    if (g_hw_rx_count > 0) {
        g_test_uart.FIFOSTS &= ~NUC472_UART_FIFOSTS_RXEMPTY;
    }
}

/* 包含被测驱动源码 */
#define NUC472_UART0 (&g_test_uart)
#include "../../drivers/serial/uart_nuc472.c"

static void reset_test_env(void)
{
    memset(&g_test_uart, 0, sizeof(g_test_uart));
    memset(&g_test_sys, 0, sizeof(g_test_sys));
    memset(&g_test_clk, 0, sizeof(g_test_clk));
    g_hw_rx_head = 0;
    g_hw_rx_tail = 0;
    g_hw_rx_count = 0;
    g_hw_tx_count = 0;
    g_in_irq = false;
    g_irq_flags = 0;

    g_test_uart.FIFOSTS = NUC472_UART_FIFOSTS_RXEMPTY | NUC472_UART_FIFOSTS_TXEMPTY;
    int rc = uart_nuc472_init(&__device_nuc472_uart0);
    assert(rc == 0);
}

/**
 * 测试 1: poll_out / poll_in 边界行为
 */
static void test_uart_poll_in_out(void)
{
    printf("[Test 1] 验证 poll_out/poll_in: FIFOSTS_TXFULL 循环等待与 FIFOSTS_RXEMPTY 返回 -EAGAIN...\n");
    reset_test_env();

    /* TXFULL 时 poll_out 应超时失败 */
    g_test_uart.FIFOSTS |= NUC472_UART_FIFOSTS_TXFULL;
    int rc = nuc472_poll_out(&__device_nuc472_uart0, 'X');
    assert(rc == -ETIMEDOUT);

    /* TX 非满时 poll_out 成功并写入 DAT */
    g_test_uart.FIFOSTS &= ~NUC472_UART_FIFOSTS_TXFULL;
    rc = nuc472_poll_out(&__device_nuc472_uart0, 'A');
    assert(rc == 0);
    assert(g_hw_tx_count == 1);
    assert(g_hw_tx_buf[0] == 'A');

    /* RXEMPTY 时 poll_in 返回 -EAGAIN */
    unsigned char ch = 0;
    g_test_uart.FIFOSTS |= NUC472_UART_FIFOSTS_RXEMPTY;
    rc = nuc472_poll_in(&__device_nuc472_uart0, &ch);
    assert(rc == -EAGAIN);

    /* 注入 1 字节后 poll_in 应成功读取 */
    uint8_t in_ch = 'K';
    hw_rx_inject(&in_ch, 1);
    rc = nuc472_poll_in(&__device_nuc472_uart0, &ch);
    assert(rc == 0);
    assert(ch == 'K');
    printf("  -> poll_out/poll_in 验证通过!\n");
}

/**
 * 测试 2: write 环形缓冲入队与 THRE 中断吐出排空
 */
static void test_uart_write_irq(void)
{
    printf("[Test 2] 验证 write: 256B TX 环形缓冲写入 32 字节开启 THREIEN，UART0_IRQHandler 排空后关闭...\n");
    reset_test_env();

    uint8_t payload[32];
    for (int i = 0; i < 32; i++) {
        payload[i] = (uint8_t)('0' + (i % 10));
    }

    /* 初始状态下 THREIEN 应为 0 */
    assert((g_test_uart.INTEN & NUC472_UART_INTEN_THREIEN) == 0);

    /* 写入 32 字节 */
    ssize_t written = nuc472_write(&__device_nuc472_uart0, payload, sizeof(payload));
    assert(written == 32);

    /* 检查 INTEN_THREIEN 自动开启 */
    assert((g_test_uart.INTEN & NUC472_UART_INTEN_THREIEN) != 0);

    /* 模拟第一次 THRE 中断，单次最多吐出 16 字节 */
    g_test_uart.INTSTS = NUC472_UART_INTSTS_THREINT;
    g_in_irq = true;
    UART0_IRQHandler();
    g_in_irq = false;

    assert(g_hw_tx_count == 16);
    assert(memcmp(g_hw_tx_buf, payload, 16) == 0);
    /* 尚未完全排空，THREIEN 应保持使能 */
    assert((g_test_uart.INTEN & NUC472_UART_INTEN_THREIEN) != 0);

    /* 模拟第二次 THRE 中断，吐出剩余 16 字节 */
    g_test_uart.INTSTS = NUC472_UART_INTSTS_THREINT;
    g_in_irq = true;
    UART0_IRQHandler();
    g_in_irq = false;

    assert(g_hw_tx_count == 32);
    assert(memcmp(g_hw_tx_buf, payload, 32) == 0);
    /* TX 队列变空，THREIEN 必须自动关闭，防止空中断风暴 */
    assert((g_test_uart.INTEN & NUC472_UART_INTEN_THREIEN) == 0);
    printf("  -> write 环形队列与 THRE 中断排空验证通过!\n");
}

/**
 * 测试 3: read 环形缓冲接收与 RLS 错误标志清理
 */
static void test_uart_read_irq_and_rls(void)
{
    printf("[Test 3] 验证 read: RDAIF/RXTOIF 排空硬件 FIFO 入环并流式读取，RLS 清除错误标志...\n");
    reset_test_env();

    uint8_t rx_data[24];
    for (int i = 0; i < 24; i++) {
        rx_data[i] = (uint8_t)('a' + i);
    }
    hw_rx_inject(rx_data, sizeof(rx_data));

    /* 触发 RDAIF 中断，将硬件 FIFO 排空至软件 RX 环形缓冲区 */
    g_test_uart.INTSTS = NUC472_UART_INTSTS_RDAINT;
    g_in_irq = true;
    UART0_IRQHandler();
    g_in_irq = false;

    /* 硬件 FIFO 已被排空 */
    assert(g_hw_rx_count == 0);
    assert((g_test_uart.FIFOSTS & NUC472_UART_FIFOSTS_RXEMPTY) != 0);

    /* 非阻塞流式读取全部 24 字节 */
    uint8_t read_buf[32];
    ssize_t n = nuc472_read(&__device_nuc472_uart0, read_buf, sizeof(read_buf), false);
    assert(n == 24);
    assert(memcmp(read_buf, rx_data, 24) == 0);

    /* 再次非阻塞读取应返回 -EAGAIN */
    n = nuc472_read(&__device_nuc472_uart0, read_buf, sizeof(read_buf), false);
    assert(n == -EAGAIN);

    /* 注入线路错误标志: RXOVIF | PEF | FEF | BIF */
    g_test_uart.FIFOSTS |= NUC472_UART_FIFOSTS_RXOVIF | NUC472_UART_FIFOSTS_PEF |
                           NUC472_UART_FIFOSTS_FEF | NUC472_UART_FIFOSTS_BIF;
    g_test_uart.INTSTS = NUC472_UART_INTSTS_RLSINT;

    g_in_irq = true;
    UART0_IRQHandler();
    g_in_irq = false;

    /* 校验错误标志已被写 1 清除 */
    uint32_t err_mask = NUC472_UART_FIFOSTS_RXOVIF | NUC472_UART_FIFOSTS_PEF |
                        NUC472_UART_FIFOSTS_FEF | NUC472_UART_FIFOSTS_BIF;
    assert((g_test_uart.FIFOSTS & err_mask) == 0);
    printf("  -> read 流式接收与 RLS 错误标志清理验证通过!\n");
}

/**
 * 测试 4: panic_begin / panic_write 紧急排空与直接轮询输出
 */
static void test_uart_panic(void)
{
    printf("[Test 4] 验证 panic_begin/panic_write: 关中断、排空 TX 环残留字符并轮询输出...\n");
    reset_test_env();

    /* 先向 TX 环写入未发字符 */
    const unsigned char pending[] = "PENDING_";
    ssize_t w = nuc472_write(&__device_nuc472_uart0, pending, strlen((const char *)pending));
    assert(w == (ssize_t)strlen((const char *)pending));
    assert((g_test_uart.INTEN & NUC472_UART_INTEN_THREIEN) != 0);

    /* 进入 panic */
    nuc472_panic_begin(&__device_nuc472_uart0);
    assert(g_test_uart.INTEN == 0);

    /* 紧急输出 panic 日志 */
    const unsigned char panic_msg[] = "PANIC_OCCURRED!";
    nuc472_panic_write(&__device_nuc472_uart0, panic_msg, strlen((const char *)panic_msg));

    /* 检查硬件输出: 应先排空 PENDING_，再输出 PANIC_OCCURRED! */
    char expected[64];
    snprintf(expected, sizeof(expected), "%s%s", pending, panic_msg);
    assert(g_hw_tx_count == strlen(expected));
    assert(memcmp(g_hw_tx_buf, expected, g_hw_tx_count) == 0);
    printf("  -> panic_begin/panic_write 验证通过!\n");
}

/**
 * 测试 5: configure 与 Mode 2 波特率分频计算
 */
static void test_uart_configure(void)
{
    printf("[Test 5] 验证 configure: UART_BAUD_MODE2_DIVIDER 与 8-N-1 参数配置...\n");
    reset_test_env();

    /* 12 MHz / 115200: (12000000 + 57600) / 115200 - 2 = 104 - 2 = 102 */
    uint32_t expected_div = NUC472_UART_BAUD_MODE2_DIVIDER(12000000UL, 115200UL);
    assert(expected_div == 102);

    struct uart_config cfg = {
        .baud_rate = 115200,
        .data_bits = UART_DATA_BITS_8,
        .parity = UART_PARITY_NONE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_FLOW_NONE,
    };
    int rc = nuc472_configure(&__device_nuc472_uart0, &cfg);
    assert(rc == 0);
    assert(g_test_uart.BAUD == (NUC472_UART_BAUD_MODE2 | 102));
    assert(g_test_uart.LINE == NUC472_UART_LINE_WLS_8BIT);

    struct uart_config get_cfg;
    rc = nuc472_get_config(&__device_nuc472_uart0, &get_cfg);
    assert(rc == 0);
    assert(get_cfg.baud_rate == 115200);
    assert(get_cfg.data_bits == UART_DATA_BITS_8);
    assert(get_cfg.parity == UART_PARITY_NONE);
    assert(get_cfg.stop_bits == UART_STOP_BITS_1);
    printf("  -> configure 与波特率分频计算验证通过!\n");
}

/**
 * 测试 6: HIRC 回退时钟保留与波特率分频计算
 */
static void test_uart_init_hirc_fallback(void)
{
    printf("[Test 6] 验证 HIRC 回退: CLKSEL1=HIRC 且时钟为 22.1184MHz 时，init 不覆写为 HXT 且 BAUD 正确...\n");
    reset_test_env();

    /* 模拟时钟初始化回退到片内 HIRC (22.1184 MHz) */
    g_test_uart0_clk_hz = 22118400UL;
    g_test_clk.CLKSEL1 = (g_test_clk.CLKSEL1 & ~NUC472_CLK_CLKSEL1_UARTSEL_Msk) |
                         NUC472_CLK_CLKSEL1_UARTSEL_HIRC;

    int rc = uart_nuc472_init(&__device_nuc472_uart0);
    assert(rc == 0);

    /* 验证 CLKSEL1 保持为 UARTSEL_HIRC，绝对不能被强制覆写为 HXT */
    uint32_t uartsel = g_test_clk.CLKSEL1 & NUC472_CLK_CLKSEL1_UARTSEL_Msk;
    assert(uartsel == NUC472_CLK_CLKSEL1_UARTSEL_HIRC);

    /* 验证 BAUD Mode 2 分频值基于 22.1184 MHz 正确计算: (22118400 + 57600) / 115200 - 2 = 190 */
    uint32_t expected_div = NUC472_UART_BAUD_MODE2_DIVIDER(22118400UL, 115200UL);
    assert(expected_div == 190);
    assert((g_test_uart.BAUD & 0xFFFFUL) == 190);

    /* 恢复默认 12 MHz */
    g_test_uart0_clk_hz = 12000000UL;
    printf("  -> HIRC 回退时钟源保留与波特率计算验证通过!\n");
}

struct blocking_read_arg {
    uint8_t out_buf[16];
    ssize_t result;
    bool started;
};

static void *blocking_read_worker(void *arg)
{
    struct blocking_read_arg *a = (struct blocking_read_arg *)arg;
    a->started = true;
    a->result = nuc472_read(&__device_nuc472_uart0, a->out_buf, 10, true);
    return NULL;
}

/**
 * 测试 7: 连续非阻塞读取超过 256 字节不发生 rx_sem 溢出与阻塞唤醒无幽灵空转
 */
static void test_uart_rx_sem_overflow_and_blocking_wakeup(void)
{
    printf("[Test 7] 验证 rx_sem: 连续非阻塞读取 320 字节不溢出，二值信号量事件唤醒无幽灵空转...\n");
    reset_test_env();

    /* 校验信号量上限必须为 1 (事件二值信号量) */
    assert(uart0_data.rx_sem.limit == 1);

    /* 连续注入并进行非阻塞流式读取超过 256 字节 (10 轮，每轮 32 字节，共 320 字节) */
    uint8_t tx_chunk[32];
    uint8_t rx_chunk[32];
    for (int i = 0; i < 32; i++) {
        tx_chunk[i] = (uint8_t)(i & 0xFF);
    }

    for (int round = 0; round < 10; round++) {
        hw_rx_inject(tx_chunk, sizeof(tx_chunk));
        g_test_uart.INTSTS = NUC472_UART_INTSTS_RDAINT;
        g_in_irq = true;
        UART0_IRQHandler();
        g_in_irq = false;

        ssize_t n = nuc472_read(&__device_nuc472_uart0, rx_chunk, sizeof(rx_chunk), false);
        assert(n == (ssize_t)sizeof(tx_chunk));
        assert(memcmp(rx_chunk, tx_chunk, sizeof(tx_chunk)) == 0);
    }

    /* 320 字节全部读取完毕，环形缓冲为空 */
    assert(uart0_data.rx_count == 0);
    /* 二值信号量上限为 1，count 绝不能饱和积攒几百个幽灵令牌 */
    assert(uart0_data.rx_sem.count <= 1);

    /* 启动后台线程执行阻塞读 */
    struct blocking_read_arg arg;
    memset(&arg, 0, sizeof(arg));
    pthread_t th;
    int prc = pthread_create(&th, NULL, blocking_read_worker, &arg);
    assert(prc == 0);

    while (!arg.started) {
        usleep(1000);
    }
    usleep(10000);
    /* 此时尚未触发中断注入数据，工作线程应当处于等待中 */
    assert(arg.result == 0);

    /* 注入 10 字节新数据并触发中断 */
    uint8_t test_bytes[10] = "1234567890";
    hw_rx_inject(test_bytes, sizeof(test_bytes));
    g_test_uart.INTSTS = NUC472_UART_INTSTS_RDAINT;
    g_in_irq = true;
    UART0_IRQHandler();
    g_in_irq = false;

    /* 等待后台工作线程被正确唤醒并返回 */
    pthread_join(th, NULL);
    assert(arg.result == 10);
    assert(memcmp(arg.out_buf, test_bytes, 10) == 0);
    assert(uart0_data.rx_count == 0);

    /* 再次非阻塞读取应返回 -EAGAIN */
    uint8_t dummy[1];
    ssize_t final_n = nuc472_read(&__device_nuc472_uart0, dummy, sizeof(dummy), false);
    assert(final_n == -EAGAIN);
    printf("  -> rx_sem 二值信号量防溢出与阻塞精准唤醒验证通过!\n");
}

int main(void)
{
    printf("=== 开始运行 NUC472 UART0 驱动中断与 FIFO 主机测试 ===\n");
    test_uart_poll_in_out();
    test_uart_write_irq();
    test_uart_read_irq_and_rls();
    test_uart_panic();
    test_uart_configure();
    test_uart_init_hirc_fallback();
    test_uart_rx_sem_overflow_and_blocking_wakeup();
    printf("====================================================\n");
    printf("所有 NUC472 UART0 单元测试全部通过!\n");
    return 0;
}
