/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 GPIO/EXTI、SPI0 与 I2C3 状态机主机回归测试 (TDD)
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUC472_HOST_TEST 1
#include <soc/nuc472_regs.h>
#include <soc/nuc472_periph.h>

/* 模拟片上外设硬件寄存器实例 */
static NUC472_GPIO_Type g_test_gpio[9];
static NUC472_SPI_Type  g_test_spi0;
static NUC472_I2C_Type  g_test_i2c3;
static NUC472_SYS_Type  g_test_sys;
static NUC472_CLK_Type  g_test_clk;

#define NUC472_SPI0       (&g_test_spi0)
#define NUC472_I2C3       (&g_test_i2c3)
#define NUC472_GPIO(port) (&g_test_gpio[port])

#include <soc/nuc472_soc.h>
#include <kernel/types.h>
#include <kernel/errno.h>
#include <kernel/device.h>
#include <drivers/gpio_nuc472.h>
#include <drivers/spi.h>
#include <drivers/i2c.h>

/* 辅助宏：用于主机测试向硬件只读（__I 即 const volatile）寄存器注入模拟值 */
#define HOST_REG_WRITE(reg_member, val) \
    (*(volatile uint32_t *)(uintptr_t)&(reg_member) = (uint32_t)(val))

NUC472_SYS_Type *nuc472_test_sys(void)
{
    return &g_test_sys;
}

NUC472_CLK_Type *nuc472_test_clk(void)
{
    return &g_test_clk;
}

NUC472_FMC_Type *nuc472_test_fmc(void)
{
    return NULL;
}

NUC472_RTC_Type *nuc472_test_rtc(void)
{
    return NULL;
}

uint32_t nuc472_clk_get_pclk_hz(void)
{
    return 84000000UL;
}

void host_test_gpio_write_intsrc(NUC472_GPIO_Type *gpio, uint32_t flags)
{
    /* 模拟硬件 W1C (Write-1-to-Clear) 清零行为 */
    gpio->INTSRC &= ~flags;
}

/* 模拟 SPI 数据收发桩 */
#define SPI_TEST_BUF_MAX 256
static uint8_t g_spi_tx_history[SPI_TEST_BUF_MAX];
static size_t  g_spi_tx_count = 0;
static uint8_t g_spi_rx_queue[SPI_TEST_BUF_MAX];
static size_t  g_spi_rx_count = 0;
static size_t  g_spi_rx_pos = 0;

void host_test_spi_write_tx(NUC472_SPI_Type *spi, uint8_t tx)
{
    if (g_spi_tx_count < SPI_TEST_BUF_MAX) {
        g_spi_tx_history[g_spi_tx_count++] = tx;
    }
    /* 模拟将发送数据存入 TX */
    spi->TX = tx;
    /* 模拟硬件接收：若注入队列有数据则填充，否则填默认 0xFF */
    uint8_t rx_val = 0xFF;
    if (g_spi_rx_pos < g_spi_rx_count) {
        rx_val = g_spi_rx_queue[g_spi_rx_pos++];
    }
    HOST_REG_WRITE(spi->RX, rx_val);
    /* 模拟移位完成：TX 非满，RX 非空 */
    spi->STATUS = (spi->STATUS & ~NUC472_SPI_STATUS_TXFULL) & ~NUC472_SPI_STATUS_RXEMPTY;
}

/* 模拟 I2C 状态机桩 */
enum mock_i2c_mode {
    MOCK_I2C_NORMAL = 0,
    MOCK_I2C_FLOATING_TIMEOUT,
};

static enum mock_i2c_mode g_i2c_test_mode = MOCK_I2C_NORMAL;
static uint8_t g_i2c_rx_queue[SPI_TEST_BUF_MAX];
static size_t  g_i2c_rx_count = 0;
static size_t  g_i2c_rx_pos = 0;
static uint8_t g_i2c_status_history[64];
static size_t  g_i2c_status_count = 0;
static bool    g_i2c_had_start = false;

static void host_record_i2c_status(uint8_t st)
{
    if (g_i2c_status_count < sizeof(g_i2c_status_history)) {
        g_i2c_status_history[g_i2c_status_count++] = st;
    }
}

void host_test_i2c_step(NUC472_I2C_Type *i2c)
{
    if (g_i2c_test_mode == MOCK_I2C_FLOATING_TIMEOUT) {
        /* 浮空总线模拟：SI 保持 0，无硬件中断应答，驱动应在有界循环后超时 */
        i2c->CTL &= ~NUC472_I2C_CTL_SI;
        return;
    }

    /* 正常状态机模拟 */
    uint32_t ctl = i2c->CTL;

    /* 软件清除 SI 并触发操作 */
    if (ctl & NUC472_I2C_CTL_STO) {
        /* STOP 信号 */
        g_i2c_had_start = false;
        i2c->CTL &= ~(NUC472_I2C_CTL_STA | NUC472_I2C_CTL_STO | NUC472_I2C_CTL_SI);
        return;
    }

    if (ctl & NUC472_I2C_CTL_STA) {
        /* START 或 Repeated START */
        if (!g_i2c_had_start) {
            /* 0x08: START 已发送 */
            HOST_REG_WRITE(i2c->STATUS, 0x08);
            g_i2c_had_start = true;
        } else {
            /* 0x10: Repeated START 已发送 */
            HOST_REG_WRITE(i2c->STATUS, 0x10);
        }
        host_record_i2c_status((uint8_t)i2c->STATUS);
        i2c->CTL = (ctl & ~NUC472_I2C_CTL_STA) | NUC472_I2C_CTL_SI;
        return;
    }

    /* 数据或地址传输 */
    if (i2c->STATUS == 0x08 || i2c->STATUS == 0x10) {
        /* 上一步是 START/ReSTART，此次清 SI 是发送 SLA+R/W */
        uint8_t addr_byte = (uint8_t)i2c->DAT;
        if ((addr_byte & 0x01) == 0) {
            /* SLA+W -> 0x18 ACK */
            HOST_REG_WRITE(i2c->STATUS, 0x18);
        } else {
            /* SLA+R -> 0x40 ACK */
            HOST_REG_WRITE(i2c->STATUS, 0x40);
        }
        host_record_i2c_status((uint8_t)i2c->STATUS);
        i2c->CTL |= NUC472_I2C_CTL_SI;
        return;
    }

    if (i2c->STATUS == 0x18 || i2c->STATUS == 0x28) {
        /* 写数据模式：收到下一个数据字节，回复 ACK -> 0x28 */
        HOST_REG_WRITE(i2c->STATUS, 0x28);
        host_record_i2c_status((uint8_t)i2c->STATUS);
        i2c->CTL |= NUC472_I2C_CTL_SI;
        return;
    }

    if (i2c->STATUS == 0x40 || i2c->STATUS == 0x50) {
        /* 读数据模式：读取 1 字节 */
        uint8_t val = 0xFF;
        if (g_i2c_rx_pos < g_i2c_rx_count) {
            val = g_i2c_rx_queue[g_i2c_rx_pos++];
        }
        i2c->DAT = val;
        if (ctl & NUC472_I2C_CTL_AA) {
            /* 主机发送 ACK 应答 -> 0x50 */
            HOST_REG_WRITE(i2c->STATUS, 0x50);
        } else {
            /* 主机发送 NACK 应答 -> 0x58 */
            HOST_REG_WRITE(i2c->STATUS, 0x58);
        }
        host_record_i2c_status((uint8_t)i2c->STATUS);
        i2c->CTL |= NUC472_I2C_CTL_SI;
        return;
    }
}

/* 包含被测驱动源码 */
#include "../../drivers/gpio/gpio_nuc472.c"
#include "../../drivers/spi/spi_nuc472.c"
#include "../../drivers/i2c/i2c_nuc472.c"

/* EXTI 中断触发计数 */
static uint32_t g_exti_trigger_count = 0;
static uint32_t g_exti_last_port = 0xFF;
static uint32_t g_exti_last_pin = 0xFF;

static void on_exti_test_callback(uint32_t port, uint32_t pin, void *ctx)
{
    g_exti_trigger_count++;
    g_exti_last_port = port;
    g_exti_last_pin = pin;
    assert(ctx == (void *)0x12345678UL);
}

/**
 * 测试 1: GPIO 全端口配置、读写与 PA.0 下降沿 EXTI 中断
 */
static void test_gpio_exti(void)
{
    printf("[Test 1] 验证 GPIO/EXTI: PB.10 输出电平切换与 PA.0 EXTI W1C 清中断与回调触发...\n");
    memset(g_test_gpio, 0, sizeof(g_test_gpio));
    g_exti_trigger_count = 0;

    /* 1.1 配置 PB.10 为推挽输出 */
    int ret = nuc472_gpio_config(NUC472_PORT_B, 10, NUC472_GPIO_MODE_OUTPUT);
    assert(ret == 0);
    /* 检查 GPIOB->MODE 的 bit [21:20] 是否为 01 */
    assert(((g_test_gpio[NUC472_PORT_B].MODE >> 20) & 0x03) == NUC472_GPIO_MODE_OUTPUT);

    /* 1.2 写入高低电平与翻转 */
    nuc472_gpio_write(NUC472_PORT_B, 10, 1);
    assert((g_test_gpio[NUC472_PORT_B].DOUT & (1U << 10)) != 0);

    nuc472_gpio_write(NUC472_PORT_B, 10, 0);
    assert((g_test_gpio[NUC472_PORT_B].DOUT & (1U << 10)) == 0);

    nuc472_gpio_toggle(NUC472_PORT_B, 10);
    assert((g_test_gpio[NUC472_PORT_B].DOUT & (1U << 10)) != 0);
    nuc472_gpio_toggle(NUC472_PORT_B, 10);
    assert((g_test_gpio[NUC472_PORT_B].DOUT & (1U << 10)) == 0);

    /* 1.3 模拟读取输入引脚 PIN */
    HOST_REG_WRITE(g_test_gpio[NUC472_PORT_A].PIN, (1U << 5));
    assert(nuc472_gpio_read(NUC472_PORT_A, 5) == 1);
    assert(nuc472_gpio_read(NUC472_PORT_A, 4) == 0);

    /* 1.4 配置 PA.0 下降沿中断并注册回调 */
    ret = nuc472_gpio_attach_irq(NUC472_PORT_A, 0, NUC472_GPIO_EDGE_FALLING,
                                 on_exti_test_callback, (void *)0x12345678UL);
    assert(ret == 0);
    /* INTTYPE 应为边沿触发 (bit 0 = 0) */
    assert((g_test_gpio[NUC472_PORT_A].INTTYPE & (1U << 0)) == 0);
    /* NUC472 硬件手册: INTEN[15:0] 为 FLIEN (下降沿使能), INTEN[31:16] 为 RHIEN (上升沿使能) */
    assert((g_test_gpio[NUC472_PORT_A].INTEN & (1U << 0)) != 0);
    assert((g_test_gpio[NUC472_PORT_A].INTEN & (1U << 16)) == 0);

    /* 1.4.1 配置 PA.1 上升沿中断 */
    ret = nuc472_gpio_attach_irq(NUC472_PORT_A, 1, NUC472_GPIO_EDGE_RISING,
                                 on_exti_test_callback, (void *)0x12345678UL);
    assert(ret == 0);
    assert((g_test_gpio[NUC472_PORT_A].INTEN & (1U << (1 + 16))) != 0);
    assert((g_test_gpio[NUC472_PORT_A].INTEN & (1U << 1)) == 0);

    /* 1.4.2 配置 PA.2 双边沿中断 */
    ret = nuc472_gpio_attach_irq(NUC472_PORT_A, 2,
                                 NUC472_GPIO_EDGE_RISING | NUC472_GPIO_EDGE_FALLING,
                                 on_exti_test_callback, (void *)0x12345678UL);
    assert(ret == 0);
    assert((g_test_gpio[NUC472_PORT_A].INTEN & (1U << 2)) != 0);
    assert((g_test_gpio[NUC472_PORT_A].INTEN & (1U << (2 + 16))) != 0);

    /* 1.5 置位 GPIOA->INTSRC (bit 0) 并调用 GPA_IRQHandler() */
    g_test_gpio[NUC472_PORT_A].INTSRC = (1U << 0);
    GPA_IRQHandler();

    /* 验证 INTSRC 写 1 清零 (W1C)，INTSRC 恢复为 0 */
    assert((g_test_gpio[NUC472_PORT_A].INTSRC & (1U << 0)) == 0);
    /* 验证回调准确触发了 1 次 */
    assert(g_exti_trigger_count == 1);
    assert(g_exti_last_port == NUC472_PORT_A);
    assert(g_exti_last_pin == 0);

    /* 1.6 测试 EINT0_IRQHandler 对应 PA.0 */
    g_test_gpio[NUC472_PORT_A].INTSRC = (1U << 0);
    EINT0_IRQHandler();
    assert((g_test_gpio[NUC472_PORT_A].INTSRC & (1U << 0)) == 0);
    assert(g_exti_trigger_count == 2);

    printf("  -> GPIO/EXTI 读写与中断分发验证通过!\n");
}

/**
 * 测试 2: SPI0 驱动 session_begin 时钟分频与 transceive 全双工收发
 */
static void test_spi0_bus(void)
{
    printf("[Test 2] 验证 SPI0: CLKDIV 分频计算、SS0 片选控制与 transceive 全双工移位...\n");
    memset(&g_test_spi0, 0, sizeof(g_test_spi0));
    memset(&g_test_clk, 0, sizeof(g_test_clk));
    g_spi_tx_count = 0;
    g_spi_rx_count = 0;
    g_spi_rx_pos = 0;

    struct device *spi_dev = &__device_nuc472_spi0;
    assert(spi_dev != NULL);
    int init_rc = nuc472_spi_init(spi_dev);
    assert(init_rc == 0);
    assert(device_is_ready(spi_dev));
    /* 校验 SPI0 外设时钟源显式锁定为 PCLK (0=PLL, 1=PCLK) */
    assert((g_test_clk.CLKSEL1 & NUC472_CLK_CLKSEL1_SPI0SEL_Msk) == NUC472_CLK_CLKSEL1_SPI0SEL_PCLK);

    /* 2.1 验证 session_begin: freq=1MHz, cs=0, word_size=8, mode=0 */
    struct spi_config cfg = {
        .frequency = 1000000UL,
        .chip_select = 0,
        .word_size = 8,
        .mode = 0,
    };

    const struct spi_driver_api *api = (const struct spi_driver_api *)spi_dev->api;
    assert(api != NULL);
    assert(api->session_begin != NULL);
    assert(api->session_end != NULL);
    assert(api->session_end_with_idle != NULL);
    assert(api->transceive != NULL);

    /* 验证非零片选返回 -EINVAL */
    struct spi_config cfg_invalid_cs = cfg;
    cfg_invalid_cs.chip_select = 1;
    assert(api->session_begin(spi_dev, &cfg_invalid_cs) == -EINVAL);

    int ret = api->session_begin(spi_dev, &cfg);
    assert(ret == 0);

    /* PCLK = 84MHz, freq = 1MHz -> NUC472 官方公式: f_spi = clk_src / (CLKDIV + 1)
     * 向上取整安全分频: (84000000 + 1000000 - 1) / 1000000 - 1 = 83 */
    assert(g_test_spi0.CLKDIV == 83U);
    /* 验证 SS0 被置位拉低选中从机 */
    assert((g_test_spi0.SSCTL & NUC472_SPI_SSCTL_SS0) != 0);

    /* 结束 session 并释放 CS */
    ret = api->session_end(spi_dev);
    assert(ret == 0);
    assert((g_test_spi0.SSCTL & NUC472_SPI_SSCTL_SS0) == 0);

    /* 验证 session_end_with_idle 在 BUSY 清除后释放 CS 并发送空闲时钟 */
    ret = api->session_begin(spi_dev, &cfg);
    assert(ret == 0);
    assert((g_test_spi0.SSCTL & NUC472_SPI_SSCTL_SS0) != 0);
    ret = api->session_end_with_idle(spi_dev, 2);
    assert(ret == 0);
    assert((g_test_spi0.SSCTL & NUC472_SPI_SSCTL_SS0) == 0);

    /* 2.2 验证 transceive: 发送 4 字节并全双工接收 4 字节 */
    uint8_t tx_data[4] = {0xAA, 0x55, 0x12, 0x34};
    uint8_t rx_data[4] = {0};
    uint8_t injected_rx[4] = {0x11, 0x22, 0x33, 0x44};

    /* 注入期待接收的数据 */
    memcpy(g_spi_rx_queue, injected_rx, sizeof(injected_rx));
    g_spi_rx_count = 4;
    g_spi_rx_pos = 0;
    g_spi_tx_count = 0;

    struct spi_buf buf = {
        .tx = tx_data,
        .rx = rx_data,
        .len = 4,
    };

    ret = spi_transceive(spi_dev, &cfg, &buf, 1);
    assert(ret == 0);
    assert(g_spi_tx_count == 4);
    assert(memcmp(g_spi_tx_history, tx_data, 4) == 0);
    assert(memcmp(rx_data, injected_rx, 4) == 0);

    printf("  -> SPI0 分频与全双工收发验证通过!\n");
}

/**
 * 测试 3: I2C3 驱动 7-bit 写、紧接重启读状态机与浮空超时
 */
static void test_i2c3_bus(void)
{
    printf("[Test 3] 验证 I2C3: 典型写 (0x08->0x18->0x28->STOP)、紧接重启读与浮空超时 (-ETIMEDOUT)...\n");
    memset(&g_test_i2c3, 0, sizeof(g_test_i2c3));
    g_i2c_status_count = 0;
    g_i2c_had_start = false;

    struct device *i2c_dev = &__device_nuc472_i2c3;
    assert(i2c_dev != NULL);
    int init_rc = nuc472_i2c_init(i2c_dev);
    assert(init_rc == 0);
    assert(device_is_ready(i2c_dev));
    /* NUC472 I2C 官方公式: f_i2c = PCLK / (4 * (CLKDIV + 1))
     * PCLK = 84MHz, bus_clk = 100kHz -> CLKDIV = 84000000 / (4 * 100000) - 1 = 209 */
    assert(g_test_i2c3.CLKDIV == 209U);
    assert((g_test_i2c3.CTL & NUC472_I2C_CTL_I2CEN) != 0);

    /* 3.1 典型 7-bit 写传输: START (0x08) -> SLA+W ACK (0x18) -> DATA ACK (0x28) -> STOP */
    g_i2c_test_mode = MOCK_I2C_NORMAL;
    uint8_t write_payload[2] = {0x10, 0x20};
    struct i2c_msg write_msg = {
        .buf = write_payload,
        .len = sizeof(write_payload),
        .flags = I2C_MSG_STOP,
    };

    int ret = i2c_transfer(i2c_dev, 0x50, &write_msg, 1);
    assert(ret == 0);
    assert(g_i2c_status_count >= 3);
    assert(g_i2c_status_history[0] == 0x08); /* START */
    assert(g_i2c_status_history[1] == 0x18); /* SLA+W ACK */
    assert(g_i2c_status_history[2] == 0x28); /* DATA 1 ACK */
    assert(g_i2c_status_history[3] == 0x28); /* DATA 2 ACK */

    /* 3.2 紧接重启读传输 (Write 1 字节 -> ReSTART 读 2 字节) */
    g_i2c_status_count = 0;
    g_i2c_had_start = false;
    uint8_t reg_addr = 0x05;
    uint8_t read_buf[2] = {0};
    uint8_t expected_rx[2] = {0xAA, 0x55};
    memcpy(g_i2c_rx_queue, expected_rx, 2);
    g_i2c_rx_count = 2;
    g_i2c_rx_pos = 0;

    struct i2c_msg restart_msgs[2] = {
        {
            .buf = &reg_addr,
            .len = 1,
            .flags = 0, /* 写寄存器地址，不发送 STOP */
        },
        {
            .buf = read_buf,
            .len = 2,
            .flags = I2C_MSG_READ | I2C_MSG_RESTART | I2C_MSG_STOP,
        },
    };

    ret = i2c_transfer(i2c_dev, 0x50, restart_msgs, 2);
    assert(ret == 0);
    assert(read_buf[0] == 0xAA);
    assert(read_buf[1] == 0x55);
    /* 检查状态序列: 0x08 -> 0x18 -> 0x28 -> 0x10 (ReSTART) -> 0x40 (SLA+R) -> 0x50 (DATA ACK) -> 0x58 (DATA NACK) */
    assert(g_i2c_status_count >= 7);
    assert(g_i2c_status_history[0] == 0x08);
    assert(g_i2c_status_history[1] == 0x18);
    assert(g_i2c_status_history[2] == 0x28);
    assert(g_i2c_status_history[3] == 0x10);
    assert(g_i2c_status_history[4] == 0x40);
    assert(g_i2c_status_history[5] == 0x50);
    assert(g_i2c_status_history[6] == 0x58);

    /* 3.3 浮空总线/无上拉超时测试 */
    g_i2c_test_mode = MOCK_I2C_FLOATING_TIMEOUT;
    ret = i2c_transfer(i2c_dev, 0x50, &write_msg, 1);
    assert(ret == -ETIMEDOUT);

    printf("  -> I2C3 状态机与有界超时退出验证通过!\n");
}

int main(void)
{
    printf("===== 启动 NUC472 DeviceTree 与 GPIO/EXTI、SPI0、I2C3 驱动测试 =====\n");
    test_gpio_exti();
    test_spi0_bus();
    test_i2c3_bus();
    printf("===== NUC472 Bus 驱动全套回归测试全部通过! =====\n");
    return 0;
}
