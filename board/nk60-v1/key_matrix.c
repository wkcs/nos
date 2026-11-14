/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 *
 * NK60-V1 直接 GPIO 5x14 按键矩阵硬件驱动 ("nk60_v1-key")：
 * - 行输出 KEY_X1..KEY_X5（PC14, PA4, PA8, PA15, PB14），推挽输出，扫描时高电平有效
 * - 列输入 KEY_Y1..KEY_Y14（PC0..PC13），内部下拉输入，高电平表示按键按下
 * - 在驱动内部完成 5x14 坐标到 61 键逻辑按键序号的映射，向 keyboard-server 输出标准按键位图
 */

#define pr_fmt(fmt) "[KEY]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/errno.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/gpio.h>
#include <kernel/mm.h>
#include <kernel/mutex.h>
#include <string.h>
#include <board/board.h>
#include <nos/keyboard.h>

/*
 * 兼容标准 CMSIS 32 位 BSRR 寄存器访问：
 * SPL 头文件将偏移 0x18 拆分为 16 位 BSRRL/BSRRH，这里使用 32 位结构体
 * 实现单周期原子置位/清零，避免与 GPIOB 上并行工作的 74HC595 LED 扫描产生竞态。
 */
typedef struct {
    __IO uint32_t MODER;
    __IO uint32_t OTYPER;
    __IO uint32_t OSPEEDR;
    __IO uint32_t PUPDR;
    __IO uint32_t IDR;
    __IO uint32_t ODR;
    __IO uint32_t BSRR;
    __IO uint32_t LCKR;
    __IO uint32_t AFR[2];
} nk60_v1_gpio_reg_t;

#define NK60_V1_GPIO_REG(base) ((nk60_v1_gpio_reg_t *)(base))

#define NK60_V1_KEY_NUM         61U
#define NK60_V1_KEY_ROWS        5U
#define NK60_V1_KEY_COLS        14U
#define NK60_V1_KEY_COL_MASK    0x3FFFU /* PC0..PC13 共 14 列 */
#define NK60_V1_KEY_WORDS       KEY_RAW_WORDS(NK60_V1_KEY_NUM)

#ifndef NK60_KEY_SETTLE_LOOPS
/* 行线拉高后或空行切换前的短建立循环数（3~4 周期/次 @ 84MHz，约 1.0~1.5 us） */
#define NK60_KEY_SETTLE_LOOPS   32U
#endif

#ifndef NK60_KEY_RECOVER_LOOPS
/* 检测到按键按下并拉低行线后的 RC 下拉放电恢复循环数（>= 15 us，防止相邻行鬼影串扰） */
#define NK60_KEY_RECOVER_LOOPS  420U
#endif

struct nk60_v1_key_row_pin {
    nk60_v1_gpio_reg_t *gpio;
    uint16_t pin;
};

/* 行输出引脚表：KEY_X1..KEY_X5 */
static const struct nk60_v1_key_row_pin s_nk60_v1_row_pins[NK60_V1_KEY_ROWS] = {
    { NK60_V1_GPIO_REG(GPIOC_BASE), GPIO_Pin_14 }, /* KEY_X1 */
    { NK60_V1_GPIO_REG(GPIOA_BASE), GPIO_Pin_4  }, /* KEY_X2 */
    { NK60_V1_GPIO_REG(GPIOA_BASE), GPIO_Pin_8  }, /* KEY_X3 */
    { NK60_V1_GPIO_REG(GPIOA_BASE), GPIO_Pin_15 }, /* KEY_X4 */
    { NK60_V1_GPIO_REG(GPIOB_BASE), GPIO_Pin_14 }, /* KEY_X5 */
};

/* NK60-V1 物理 5x14 行列坐标到 61 键逻辑序号的映射表（与 ANSI-61 布局一致） */
static const int8_t s_nk60_v1_position_index[NK60_V1_KEY_ROWS][NK60_V1_KEY_COLS] = {
    { 0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13},
    {14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27},
    {28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, -1, 40},
    {41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, 52},
    {53, 54, 55, -1, -1, 56, -1, -1, -1, 57, 58, 59, -1, 60},
};

struct nk60_v1_key {
    struct mutex            lock;
    struct device           dev;
    bool                    enable;
    uint16_t                debounce_ms;
    uint16_t                poll_rate_hz;
    struct key_driver_stats stats;
};

static inline void nk60_v1_delay_loops(uint32_t loops)
{
    if (loops == 0U) {
        return;
    }
    __asm__ volatile(
        "1:\n\t"
        "subs %0, %0, #1\n\t"
        "bne 1b\n\t"
        : "+r"(loops)
        :
        : "cc"
    );
}

static void nk60_v1_rows_all_low(void)
{
    uint32_t row;

    for (row = 0U; row < NK60_V1_KEY_ROWS; row++) {
        s_nk60_v1_row_pins[row].gpio->BSRR =
            (uint32_t)s_nk60_v1_row_pins[row].pin << 16U;
    }
}

static noinline void nk60_v1_key_scan_once(uint32_t *key_bitmap)
{
    uint32_t row;
    uint32_t col;
    uint32_t col_mask;
    const nk60_v1_gpio_reg_t *col_gpio = NK60_V1_GPIO_REG(GPIOC_BASE);

    memset(key_bitmap, 0, NK60_V1_KEY_WORDS * sizeof(uint32_t));

    for (row = 0U; row < NK60_V1_KEY_ROWS; row++) {
        /* 拉高当前扫描行 */
        s_nk60_v1_row_pins[row].gpio->BSRR = (uint32_t)s_nk60_v1_row_pins[row].pin;
        nk60_v1_delay_loops(NK60_KEY_SETTLE_LOOPS);

        /* 一次性读取 PC0..PC13 的列电平状态 */
        col_mask = col_gpio->IDR & NK60_V1_KEY_COL_MASK;

        /* 立即拉低当前扫描行 */
        s_nk60_v1_row_pins[row].gpio->BSRR =
            (uint32_t)s_nk60_v1_row_pins[row].pin << 16U;

        if (col_mask != 0U) {
            for (col = 0U; col < NK60_V1_KEY_COLS; col++) {
                if ((col_mask & (1UL << col)) != 0U) {
                    int8_t key_idx = s_nk60_v1_position_index[row][col];

                    if (key_idx >= 0 && (uint8_t)key_idx < NK60_V1_KEY_NUM) {
                        key_bitmap[KEY_RAW_GROUP((uint8_t)key_idx)] |=
                            (1UL << KEY_RAW_BIT((uint8_t)key_idx));
                    }
                }
            }
            /*
             * 该行存在按键按下时，行拉低后由于二极管反向截止，列寄生电容
             * 仅能依靠 STM32 内部 ~40kΩ 弱下拉放电，需等待充足恢复时间防止串扰。
             */
            nk60_v1_delay_loops(NK60_KEY_RECOVER_LOOPS);
        } else {
            nk60_v1_delay_loops(NK60_KEY_SETTLE_LOOPS);
        }
    }
}

static ssize_t nk60_v1_key_read(struct device *dev, addr_t pos, void *buffer, size_t size)
{
    struct nk60_v1_key *key;
    uint32_t *key_bitmap = (uint32_t *)buffer;
    size_t copy_bytes = NK60_V1_KEY_WORDS * sizeof(uint32_t);
    uint32_t w;

    (void)pos;
    if (dev == NULL || buffer == NULL || size < copy_bytes) {
        return -EINVAL;
    }

    key = dev->priv;
    if (key == NULL) {
        return -EINVAL;
    }
    if (!key->enable) {
        memset(buffer, 0, copy_bytes);
        return (ssize_t)copy_bytes;
    }

    mutex_lock(&key->lock);
    nk60_v1_key_scan_once(key_bitmap);
    key->stats.scan_count++;

    for (w = 0U; w < NK60_V1_KEY_WORDS; w++) {
        if (key_bitmap[w] != 0U) {
            key->stats.key_down_events++;
            break;
        }
    }
    mutex_unlock(&key->lock);

    return (ssize_t)copy_bytes;
}

static int nk60_v1_key_control(struct device *dev, int cmd, void *args)
{
    struct nk60_v1_key *key;
    struct key_dev_info info;

    if (dev == NULL || dev->priv == NULL) {
        return -EINVAL;
    }
    key = dev->priv;

    switch (cmd) {
    case KEY_CTRL_ENABLE:
        mutex_lock(&key->lock);
        nk60_v1_rows_all_low();
        key->enable = true;
        mutex_unlock(&key->lock);
        break;
    case KEY_CTRL_DISABLE:
        mutex_lock(&key->lock);
        key->enable = false;
        nk60_v1_rows_all_low();
        mutex_unlock(&key->lock);
        break;
    case KEY_CTRL_GET_ENABLE_STATUS:
        return key->enable ? 1 : 0;
    case KEY_CTRL_GET_DEV_INFO:
        if (args == NULL) {
            return -EINVAL;
        }
        info.key_count = NK60_V1_KEY_NUM;
        info.debounce_ms = key->debounce_ms;
        info.poll_rate_hz = key->poll_rate_hz;
        memcpy(args, &info, sizeof(info));
        break;
    case KEY_CTRL_SET_DEBOUNCE:
        if (args == NULL) {
            return -EINVAL;
        }
        key->debounce_ms = *(const uint16_t *)args;
        break;
    case KEY_CTRL_SET_POLL_RATE:
        if (args == NULL) {
            return -EINVAL;
        }
        key->poll_rate_hz = *(const uint16_t *)args;
        break;
    case KEY_CTRL_GET_STATS:
        if (args == NULL) {
            return -EINVAL;
        }
        memcpy(args, &key->stats, sizeof(struct key_driver_stats));
        break;
    default:
        pr_err("unknown cmd: %d\n", cmd);
        return -EINVAL;
    }

    return 0;
}

static void nk60_v1_key_gpio_init(void)
{
    GPIO_InitTypeDef gpio_init;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA | RCC_AHB1Periph_GPIOB | RCC_AHB1Periph_GPIOC,
                           ENABLE);

    /* 先将所有行输出锁存预置为低电平，再切换为输出模式避免上电毛刺 */
    nk60_v1_rows_all_low();

    /* 行输出配置：PA4, PA8, PA15 (KEY_X2..KEY_X4) */
    gpio_init.GPIO_Pin = GPIO_Pin_4 | GPIO_Pin_8 | GPIO_Pin_15;
    gpio_init.GPIO_Mode = GPIO_Mode_OUT;
    gpio_init.GPIO_OType = GPIO_OType_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_100MHz;
    gpio_init.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOA, &gpio_init);

    /* 行输出配置：PB14 (KEY_X5) */
    gpio_init.GPIO_Pin = GPIO_Pin_14;
    GPIO_Init(GPIOB, &gpio_init);

    /* 行输出配置：PC14 (KEY_X1) */
    gpio_init.GPIO_Pin = GPIO_Pin_14;
    GPIO_Init(GPIOC, &gpio_init);

    /* 列输入配置：PC0..PC13 (KEY_Y1..KEY_Y14)，内部下拉，高电平触发 */
    gpio_init.GPIO_Pin = NK60_V1_KEY_COL_MASK;
    gpio_init.GPIO_Mode = GPIO_Mode_IN;
    gpio_init.GPIO_OType = GPIO_OType_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_100MHz;
    gpio_init.GPIO_PuPd = GPIO_PuPd_DOWN;
    GPIO_Init(GPIOC, &gpio_init);

    nk60_v1_rows_all_low();
}

static int nk60_v1_key_init(void)
{
    struct nk60_v1_key *key;

    key = kmalloc(sizeof(struct nk60_v1_key), GFP_KERNEL);
    if (key == NULL) {
        pr_err("alloc nk60_v1_key error\n");
        return -ENOMEM;
    }

    nk60_v1_key_gpio_init();
    mutex_init(&key->lock);
    key->enable = true;
    key->debounce_ms = 10U;
    key->poll_rate_hz = 100U;
    memset(&key->stats, 0, sizeof(key->stats));

    device_init(&key->dev);
    key->dev.name = "nk60_v1-key";
    key->dev.ops.read = nk60_v1_key_read;
    key->dev.ops.control = nk60_v1_key_control;
    key->dev.priv = key;
    device_register(&key->dev);

    return 0;
}
task_init(nk60_v1_key_init);
