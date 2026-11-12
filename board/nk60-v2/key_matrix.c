/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 *
 * NK60-V2 74HC595 Shift-Register Key Matrix Hardware Driver ("nk60_v2-key").
 * Scans HC595_BIT_NUM bits, maps raw hardware scan bits to logical key indices
 * using driver-internal position mapping, and outputs standard logical key bitmaps.
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

#define HC595_EN_PIN    GPIO_Pin_12
#define HC595_CLK_PIN   GPIO_Pin_13
#define KEY_DATA_PIN    GPIO_Pin_14
#define HC595_DATA_PIN  GPIO_Pin_15

/*
 * 兼容标准 CMSIS 32 位 BSRR 寄存器访问：
 * 厂商 SPL 库头文件将 0x18 拆分为 16 位的 BSRRL/BSRRH，
 * 此处为本文件重定义 GPIOB 寄存器结构以支持 32 位原子单周期写入。
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
} nk60_gpio_reg_t;

#undef GPIOB
#define GPIOB ((nk60_gpio_reg_t *)GPIOB_BASE)

#define NK60_KEY_NUM        61U
#define HC595_BIT_NUM       (8 * 9)
#define HC595_RAW_DATA_NUM  KEY_RAW_WORDS(HC595_BIT_NUM)
#define NK60_KEY_WORDS      KEY_RAW_WORDS(NK60_KEY_NUM)

#ifndef NK60_KEY_SETTLE_LOOPS
/* 上一位未拉低时的短建立循环数（3~4 周期/次 @ 84MHz，约 1.0~1.3 us） */
#define NK60_KEY_SETTLE_LOOPS 28U
#endif

#ifndef NK60_KEY_RECOVER_LOOPS
/* 上一位被拉低后的 RC 上拉恢复循环数（3~4 周期/次 @ 84MHz，>= 15 us） */
#define NK60_KEY_RECOVER_LOOPS 420U
#endif

/* 74HC595 硬件扫描位到 NK60 逻辑按键序号的私有走线映射表 */
static const int8_t s_nk60_position_index[HC595_BIT_NUM] = {
     0,  1,  2,  3,  4,  5,  6,  7,
     8,  9, 10, 11, 12, 13, -1, -1,
    14, 15, 16, 17, 18, 19, 20, 21,
    22, 23, 24, 25, 26, 27, -1, -1,
    36, 37, 38, 39, 40, -1, -1, -1,
    49, 50, 51, 52, -1, -1, -1, -1,
    53, 54, 55, 56, 57, 58, 59, 60,
    41, 42, 43, 44, 45, 46, 47, 48,
    28, 29, 30, 31, 32, 33, 34, 35
};

struct nk60_key {
    struct mutex            lock;
    struct device           dev;
    bool                    enable;
    uint16_t                debounce_ms;
    uint16_t                poll_rate_hz;
    struct key_driver_stats stats;
};

static inline void hc595_pulse_clk(void)
{
    GPIOB->BSRR = HC595_CLK_PIN;
    __asm__ volatile("nop\n\tnop\n\tnop\n\tnop\n\t");
    GPIOB->BSRR = (uint32_t)HC595_CLK_PIN << 16U;
}

static inline void hc595_pulse_load(void)
{
    GPIOB->BSRR = HC595_EN_PIN;
    __asm__ volatile("nop\n\tnop\n\tnop\n\tnop\n\t");
    GPIOB->BSRR = (uint32_t)HC595_EN_PIN << 16U;
}

static inline void nk60_delay_loops(uint32_t loops)
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

static noinline void nk60_v2_key_scan_once(uint32_t *raw_data)
{
    int i;
    bool prev_low;

    memset(raw_data, 0, HC595_RAW_DATA_NUM * sizeof(uint32_t));

    /*
     * 若扫描开始前 PB14 仍处于低电平（例如上一轮末尾位 71 按键被按下），
     * 首位切换后同样需要等待完整的 RC 上拉恢复时间，避免误判串扰到第 0 位。
     */
    prev_low = ((GPIOB->IDR & KEY_DATA_PIN) == 0U);

    for (i = 0; i < (int)HC595_BIT_NUM; i++) {
        if (i == 0) {
            GPIOB->BSRR = (uint32_t)HC595_DATA_PIN << 16U; /* 送 0 */
        } else {
            GPIOB->BSRR = HC595_DATA_PIN;                  /* 送 1 */
        }
        __asm__ volatile("nop\n\tnop\n\tnop\n\tnop\n\t");

        hc595_pulse_clk();
        hc595_pulse_load();

        /*
         * 按键未按下时 PB14 保持高电平，仅需短暂等待主动下拉建立；
         * 若上一位刚将 PB14 拉低，因二极管反向截止，只能依靠 40kΩ 弱上拉
         * 对矩阵寄生电容缓慢充电，必须给予 >= 15us 恢复延时防止相邻键位串扰。
         */
        if (prev_low) {
            nk60_delay_loops(NK60_KEY_RECOVER_LOOPS);
        } else {
            nk60_delay_loops(NK60_KEY_SETTLE_LOOPS);
        }

        if ((GPIOB->IDR & KEY_DATA_PIN) == 0U) {
            raw_data[KEY_RAW_GROUP(i)] |= (1UL << KEY_RAW_BIT(i));
            prev_low = true;
        } else {
            prev_low = false;
        }
    }
}

static ssize_t nk60_v2_key_read(struct device *dev, addr_t pos, void *buffer, size_t size)
{
    struct nk60_key *key = dev->priv;
    uint32_t raw_data[HC595_RAW_DATA_NUM];
    uint32_t *key_bitmap = (uint32_t *)buffer;
    size_t copy_bytes = NK60_KEY_WORDS * sizeof(uint32_t);
    int bit;
    int w;

    (void)pos;
    if (key == NULL || buffer == NULL || size < copy_bytes) {
        return -EINVAL;
    }
    if (!key->enable) {
        memset(buffer, 0, copy_bytes);
        return (ssize_t)copy_bytes;
    }

    mutex_lock(&key->lock);
    nk60_v2_key_scan_once(raw_data);
    key->stats.scan_count++;

    /* 在驱动内部将 74HC595 硬件扫描位转换为规范的逻辑按键位图 */
    memset(key_bitmap, 0, copy_bytes);
    for (bit = 0; bit < HC595_BIT_NUM; bit++) {
        if ((raw_data[KEY_RAW_GROUP(bit)] >> KEY_RAW_BIT(bit)) & 0x01UL) {
            int8_t key_idx = s_nk60_position_index[bit];
            if (key_idx >= 0 && (uint8_t)key_idx < NK60_KEY_NUM) {
                key_bitmap[KEY_RAW_GROUP((uint8_t)key_idx)] |=
                    (1UL << KEY_RAW_BIT((uint8_t)key_idx));
            }
        }
    }

    for (w = 0; w < (int)NK60_KEY_WORDS; w++) {
        if (key_bitmap[w] != 0U) {
            key->stats.key_down_events++;
            break;
        }
    }
    mutex_unlock(&key->lock);
    return (ssize_t)copy_bytes;
}

static int nk60_v2_key_control(struct device *dev, int cmd, void *args)
{
    struct nk60_key *key = dev->priv;
    struct key_dev_info info;

    if (key == NULL) {
        return -EINVAL;
    }

    switch (cmd) {
    case KEY_CTRL_ENABLE:
        key->enable = true;
        break;
    case KEY_CTRL_DISABLE:
        key->enable = false;
        GPIOB->BSRR = ((uint32_t)HC595_CLK_PIN << 16U) |
                      ((uint32_t)HC595_DATA_PIN << 16U) |
                      ((uint32_t)HC595_EN_PIN << 16U);
        break;
    case KEY_CTRL_GET_ENABLE_STATUS:
        return key->enable;
    case KEY_CTRL_GET_DEV_INFO:
        if (args == NULL) {
            return -EINVAL;
        }
        info.key_count = NK60_KEY_NUM;
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

static int nk60_v2_key_gpio_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_12 | GPIO_Pin_13 | GPIO_Pin_15;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init((GPIO_TypeDef *)GPIOB, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_14;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init((GPIO_TypeDef *)GPIOB, &GPIO_InitStructure);

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_IN;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOA, &GPIO_InitStructure);

    GPIOB->BSRR = ((uint32_t)HC595_CLK_PIN << 16U) |
                  ((uint32_t)HC595_DATA_PIN << 16U) |
                  ((uint32_t)HC595_EN_PIN << 16U);

    return 0;
}

static int nk60_v2_key_init(void)
{
    struct nk60_key *key;

    key = kmalloc(sizeof(struct nk60_key), GFP_KERNEL);
    if (key == NULL) {
        pr_err("alloc nk60_key error\n");
        return -ENOMEM;
    }

    nk60_v2_key_gpio_init();
    mutex_init(&key->lock);
    key->enable = true;
    key->debounce_ms = 10U;
    key->poll_rate_hz = 100U;
    memset(&key->stats, 0, sizeof(key->stats));

    device_init(&key->dev);
    key->dev.name = "nk60_v2-key";
    key->dev.ops.read = nk60_v2_key_read;
    key->dev.ops.control = nk60_v2_key_control;
    key->dev.priv = key;
    device_register(&key->dev);

    return 0;
}
task_init(nk60_v2_key_init);
