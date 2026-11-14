/**
 * Copyright (C) 2023-2026 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 *
 * NK60-V1 RGB 背光矩阵驱动 ("nk60_v1-led")：
 * - 15 行（5 个按键行 × R/G/B 3 路）采用级联 74HC595（PB12=EN/RCLK, PB13=CLK, PB15=DATA）分时扫描
 * - 14 列采用 TIM2/TIM3/TIM4/TIM5 共 14 路 10-bit PWM（低电平有效、Period=1023、Prescaler=1）控制亮度
 * - 内置 Gamma 1.6 查找表与 R/G/B 通道白平衡增益，兼顾矩阵亮度、人眼线性度及三色灯珠差异
 * - TIM9 定时中断（TIM1_BRK_TIM9_IRQHandler，约 2 kHz 行扫描）逐行消隐、移位锁存并更新列 PWM
 */

#define pr_fmt(fmt) "[LED]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/types.h>
#include <kernel/errno.h>
#include <kernel/device.h>
#include <kernel/irq.h>
#include <kernel/init.h>
#include <kernel/gpio.h>
#include <kernel/mm.h>
#include <kernel/mutex.h>
#include <string.h>
#include <board/board.h>

#include <drivers/led.h>
#include <nos/display.h>

/* 与按键驱动一致的 32 位原子 BSRR 访问结构体，保证 GPIOB PB14 不受 74HC595 中断写操作影响 */
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
} nk60_v1_led_gpio_reg_t;

#define NK60_V1_GPIOB_REG       ((nk60_v1_led_gpio_reg_t *)GPIOB_BASE)

#define HC595_EN_PIN            GPIO_Pin_12
#define HC595_CLK_PIN           GPIO_Pin_13
#define HC595_DATA_PIN          GPIO_Pin_15

#define LED_WIDTH               14U
#define LED_HEIGHT              5U
#define LED_SCAN_ROWS           (LED_HEIGHT * 3U) /* 共 15 个扫描行：每按键行 3 路 (R, G, B) */
#define LED_PWM_PERIOD          1023U
#define LED_PWM_PRESCALER       1U

/*
 * R/G/B 通道白平衡增益（Q8 定点数，256 = 100%）：
 * 分立 RGB 灯珠的红光 Vf 较低且发光效率偏高，等占空比下白色会偏粉。
 * 实机微调建议：固定最弱通道为 256，显示纯白 (255,255,255) 时先下调 R，再微调 B。
 */
#define LED_WB_GAIN_R           152U
#define LED_WB_GAIN_G           256U
#define LED_WB_GAIN_B           176U
#define LED_WB_UNITY            256U

/* TIM9 行扫描参数：APB2 定时器时钟 84 MHz / (499 + 1) / (83 + 1) = 2000 Hz */
#define LED_TIM9_PERIOD         83U
#define LED_TIM9_PRESCALER      499U

struct nk60_v1_pwm_pin {
    GPIO_TypeDef *gpio;
    uint16_t      pin;
    uint8_t       pin_source;
    uint8_t       af;
};

/* 14 列 PWM 输出引脚映射表，顺序严格对应键盘第 0..13 列 */
static const struct nk60_v1_pwm_pin s_pwm_pins[LED_WIDTH] = {
    { GPIOA, GPIO_Pin_0,  GPIO_PinSource0,  GPIO_AF_TIM5 }, /* Col 0:  TIM5_CH1 */
    { GPIOA, GPIO_Pin_1,  GPIO_PinSource1,  GPIO_AF_TIM5 }, /* Col 1:  TIM5_CH2 */
    { GPIOA, GPIO_Pin_2,  GPIO_PinSource2,  GPIO_AF_TIM5 }, /* Col 2:  TIM5_CH3 */
    { GPIOA, GPIO_Pin_3,  GPIO_PinSource3,  GPIO_AF_TIM5 }, /* Col 3:  TIM5_CH4 */
    { GPIOA, GPIO_Pin_6,  GPIO_PinSource6,  GPIO_AF_TIM3 }, /* Col 4:  TIM3_CH1 */
    { GPIOA, GPIO_Pin_7,  GPIO_PinSource7,  GPIO_AF_TIM3 }, /* Col 5:  TIM3_CH2 */
    { GPIOB, GPIO_Pin_0,  GPIO_PinSource0,  GPIO_AF_TIM3 }, /* Col 6:  TIM3_CH3 */
    { GPIOB, GPIO_Pin_1,  GPIO_PinSource1,  GPIO_AF_TIM3 }, /* Col 7:  TIM3_CH4 */
    { GPIOB, GPIO_Pin_6,  GPIO_PinSource6,  GPIO_AF_TIM4 }, /* Col 8:  TIM4_CH1 */
    { GPIOB, GPIO_Pin_7,  GPIO_PinSource7,  GPIO_AF_TIM4 }, /* Col 9:  TIM4_CH2 */
    { GPIOB, GPIO_Pin_8,  GPIO_PinSource8,  GPIO_AF_TIM4 }, /* Col 10: TIM4_CH3 */
    { GPIOB, GPIO_Pin_9,  GPIO_PinSource9,  GPIO_AF_TIM4 }, /* Col 11: TIM4_CH4 */
    { GPIOA, GPIO_Pin_5,  GPIO_PinSource5,  GPIO_AF_TIM2 }, /* Col 12: TIM2_CH1 */
    { GPIOB, GPIO_Pin_10, GPIO_PinSource10, GPIO_AF_TIM2 }, /* Col 13: TIM2_CH3 */
};

/* 14 列对应的定时器捕获比较寄存器指针 */
static __IO uint32_t *const s_pwm_ccr[LED_WIDTH] = {
    &TIM5->CCR1, &TIM5->CCR2, &TIM5->CCR3, &TIM5->CCR4,
    &TIM3->CCR1, &TIM3->CCR2, &TIM3->CCR3, &TIM3->CCR4,
    &TIM4->CCR1, &TIM4->CCR2, &TIM4->CCR3, &TIM4->CCR4,
    &TIM2->CCR1, &TIM2->CCR3,
};

struct nk60_v1_led {
    struct mutex  lock;
    struct device dev;
    bool          enable;
};

static uint16_t s_display_buf[LED_SCAN_ROWS][LED_WIDTH];
static volatile uint8_t s_scan_row;

/* sRGB 8-bit -> 10-bit 线性光强查找表 (Gamma 1.6)，非零输入最小保留 1 以避免暗部死区 */
static const uint16_t s_gamma_lut[256] = {
       0U,    1U,    1U,    1U,    1U,    2U,    3U,    3U,    4U,    5U,    6U,    7U,    8U,    9U,   10U,   11U,
      12U,   13U,   15U,   16U,   17U,   19U,   20U,   22U,   23U,   25U,   27U,   28U,   30U,   32U,   33U,   35U,
      37U,   39U,   41U,   43U,   45U,   47U,   49U,   51U,   53U,   55U,   57U,   59U,   62U,   64U,   66U,   68U,
      71U,   73U,   75U,   78U,   80U,   83U,   85U,   88U,   90U,   93U,   96U,   98U,  101U,  104U,  106U,  109U,
     112U,  115U,  118U,  121U,  123U,  126U,  129U,  132U,  135U,  138U,  141U,  144U,  147U,  151U,  154U,  157U,
     160U,  163U,  167U,  170U,  173U,  176U,  180U,  183U,  186U,  190U,  193U,  197U,  200U,  204U,  207U,  211U,
     214U,  218U,  221U,  225U,  229U,  232U,  236U,  240U,  244U,  247U,  251U,  255U,  259U,  263U,  266U,  270U,
     274U,  278U,  282U,  286U,  290U,  294U,  298U,  302U,  306U,  310U,  314U,  319U,  323U,  327U,  331U,  335U,
     340U,  344U,  348U,  352U,  357U,  361U,  365U,  370U,  374U,  379U,  383U,  387U,  392U,  396U,  401U,  405U,
     410U,  415U,  419U,  424U,  428U,  433U,  438U,  442U,  447U,  452U,  457U,  461U,  466U,  471U,  476U,  480U,
     485U,  490U,  495U,  500U,  505U,  510U,  515U,  520U,  525U,  530U,  535U,  540U,  545U,  550U,  555U,  560U,
     565U,  570U,  576U,  581U,  586U,  591U,  596U,  602U,  607U,  612U,  617U,  623U,  628U,  634U,  639U,  644U,
     650U,  655U,  661U,  666U,  671U,  677U,  682U,  688U,  694U,  699U,  705U,  710U,  716U,  721U,  727U,  733U,
     738U,  744U,  750U,  756U,  761U,  767U,  773U,  779U,  784U,  790U,  796U,  802U,  808U,  814U,  820U,  825U,
     831U,  837U,  843U,  849U,  855U,  861U,  867U,  873U,  879U,  885U,  892U,  898U,  904U,  910U,  916U,  922U,
     928U,  935U,  941U,  947U,  953U,  960U,  966U,  972U,  978U,  985U,  991U,  997U, 1004U, 1010U, 1017U, 1023U,
};

static inline uint16_t nk60_v1_led_map_channel(uint8_t val, uint16_t gain)
{
    uint32_t linear;

    if (val == 0U || gain == 0U) {
        return 0U;
    }
    linear = ((uint32_t)s_gamma_lut[val] * gain + (LED_WB_UNITY / 2U)) / LED_WB_UNITY;
    if (linear == 0U) {
        return 1U;
    }
    if (linear > LED_PWM_PERIOD) {
        return LED_PWM_PERIOD;
    }
    return (uint16_t)linear;
}

static inline void nk60_v1_hc595_delay(void)
{
    /* @84MHz 约 95ns，远高于 SN74HC595 在 3.3V 下的最小脉宽及建立时间 */
    __asm__ volatile(
        "nop\n\tnop\n\tnop\n\tnop\n\t"
        "nop\n\tnop\n\tnop\n\tnop\n\t"
    );
}

static inline void nk60_v1_hc595_set_pin(uint16_t pin, bool high)
{
    NK60_V1_GPIOB_REG->BSRR = high ? (uint32_t)pin : ((uint32_t)pin << 16U);
}

static inline void nk60_v1_hc595_load(void)
{
    nk60_v1_hc595_set_pin(HC595_EN_PIN, false);
    nk60_v1_hc595_delay();
    nk60_v1_hc595_set_pin(HC595_EN_PIN, true);
    nk60_v1_hc595_delay();
}

static void nk60_v1_hc595_send_word(uint16_t data)
{
    uint32_t i;

    for (i = 0U; i < 16U; i++) {
        nk60_v1_hc595_set_pin(HC595_DATA_PIN, (data & 0x8000U) != 0U);
        nk60_v1_hc595_set_pin(HC595_CLK_PIN, false);
        nk60_v1_hc595_delay();
        data <<= 1U;
        nk60_v1_hc595_set_pin(HC595_CLK_PIN, true);
        nk60_v1_hc595_delay();
    }
    nk60_v1_hc595_load();
}

static inline void nk60_v1_hc595_send_bit(bool bit_val)
{
    nk60_v1_hc595_set_pin(HC595_DATA_PIN, bit_val);
    nk60_v1_hc595_set_pin(HC595_CLK_PIN, false);
    nk60_v1_hc595_delay();
    nk60_v1_hc595_set_pin(HC595_CLK_PIN, true);
    nk60_v1_hc595_delay();
    nk60_v1_hc595_load();
}

static inline void nk60_v1_pwm_sync_update(void)
{
    /* 立即将影子寄存器 CCR 装载到激活寄存器并清零计数器，保证消隐与 4 个定时器相位同步 */
    TIM2->EGR = TIM_EGR_UG;
    TIM3->EGR = TIM_EGR_UG;
    TIM4->EGR = TIM_EGR_UG;
    TIM5->EGR = TIM_EGR_UG;
}

static inline void nk60_v1_pwm_set_led_off(void)
{
    uint32_t col;

    for (col = 0U; col < LED_WIDTH; col++) {
        *s_pwm_ccr[col] = 0U;
    }
    nk60_v1_pwm_sync_update();
}

static inline void nk60_v1_pwm_set_led_data(const uint16_t buf[LED_WIDTH])
{
    uint32_t col;

    for (col = 0U; col < LED_WIDTH; col++) {
        *s_pwm_ccr[col] = buf[col];
    }
    nk60_v1_pwm_sync_update();
}

static void nk60_v1_pwm_init(void)
{
    GPIO_InitTypeDef gpio_init;
    TIM_TimeBaseInitTypeDef tb_init;
    TIM_OCInitTypeDef oc_init;
    TIM_TypeDef *const timers[] = { TIM2, TIM3, TIM4, TIM5 };
    uint32_t i;

    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2 | RCC_APB1Periph_TIM3 |
                           RCC_APB1Periph_TIM4 | RCC_APB1Periph_TIM5, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA | RCC_AHB1Periph_GPIOB, ENABLE);

    /* 配置 14 路 PWM 输出引脚复用功能 */
    gpio_init.GPIO_Mode = GPIO_Mode_AF;
    gpio_init.GPIO_Speed = GPIO_Speed_100MHz;
    gpio_init.GPIO_OType = GPIO_OType_PP;
    gpio_init.GPIO_PuPd = GPIO_PuPd_UP;
    for (i = 0U; i < LED_WIDTH; i++) {
        GPIO_PinAFConfig(s_pwm_pins[i].gpio, s_pwm_pins[i].pin_source, s_pwm_pins[i].af);
        gpio_init.GPIO_Pin = s_pwm_pins[i].pin;
        GPIO_Init(s_pwm_pins[i].gpio, &gpio_init);
    }

    /* 配置 TIM2/3/4/5 时基：84MHz / 2 / 1024 ≈ 41.02kHz 10-bit PWM，Period = 1023 */
    TIM_TimeBaseStructInit(&tb_init);
    tb_init.TIM_Prescaler = LED_PWM_PRESCALER;
    tb_init.TIM_CounterMode = TIM_CounterMode_Up;
    tb_init.TIM_Period = LED_PWM_PERIOD;
    tb_init.TIM_ClockDivision = TIM_CKD_DIV1;
    for (i = 0U; i < (uint32_t)(sizeof(timers) / sizeof(timers[0])); i++) {
        TIM_TimeBaseInit(timers[i], &tb_init);
    }

    /* 配置输出比较为 PWM1 低电平有效，初始占空比为 0 */
    TIM_OCStructInit(&oc_init);
    oc_init.TIM_OCMode = TIM_OCMode_PWM1;
    oc_init.TIM_OutputState = TIM_OutputState_Enable;
    oc_init.TIM_Pulse = 0U;
    oc_init.TIM_OCPolarity = TIM_OCPolarity_Low;

    TIM_OC1Init(TIM2, &oc_init);
    TIM_OC3Init(TIM2, &oc_init);
    TIM_OC1PreloadConfig(TIM2, TIM_OCPreload_Enable);
    TIM_OC3PreloadConfig(TIM2, TIM_OCPreload_Enable);

    TIM_OC1Init(TIM3, &oc_init);
    TIM_OC2Init(TIM3, &oc_init);
    TIM_OC3Init(TIM3, &oc_init);
    TIM_OC4Init(TIM3, &oc_init);
    TIM_OC1PreloadConfig(TIM3, TIM_OCPreload_Enable);
    TIM_OC2PreloadConfig(TIM3, TIM_OCPreload_Enable);
    TIM_OC3PreloadConfig(TIM3, TIM_OCPreload_Enable);
    TIM_OC4PreloadConfig(TIM3, TIM_OCPreload_Enable);

    TIM_OC1Init(TIM4, &oc_init);
    TIM_OC2Init(TIM4, &oc_init);
    TIM_OC3Init(TIM4, &oc_init);
    TIM_OC4Init(TIM4, &oc_init);
    TIM_OC1PreloadConfig(TIM4, TIM_OCPreload_Enable);
    TIM_OC2PreloadConfig(TIM4, TIM_OCPreload_Enable);
    TIM_OC3PreloadConfig(TIM4, TIM_OCPreload_Enable);
    TIM_OC4PreloadConfig(TIM4, TIM_OCPreload_Enable);

    TIM_OC1Init(TIM5, &oc_init);
    TIM_OC2Init(TIM5, &oc_init);
    TIM_OC3Init(TIM5, &oc_init);
    TIM_OC4Init(TIM5, &oc_init);
    TIM_OC1PreloadConfig(TIM5, TIM_OCPreload_Enable);
    TIM_OC2PreloadConfig(TIM5, TIM_OCPreload_Enable);
    TIM_OC3PreloadConfig(TIM5, TIM_OCPreload_Enable);
    TIM_OC4PreloadConfig(TIM5, TIM_OCPreload_Enable);

    for (i = 0U; i < (uint32_t)(sizeof(timers) / sizeof(timers[0])); i++) {
        TIM_ARRPreloadConfig(timers[i], ENABLE);
        TIM_Cmd(timers[i], ENABLE);
    }

    nk60_v1_pwm_set_led_off();
}

static void nk60_v1_tim9_init(void)
{
    TIM_TimeBaseInitTypeDef tb_init;
    NVIC_InitTypeDef nvic_init;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM9, ENABLE);

    TIM_TimeBaseStructInit(&tb_init);
    tb_init.TIM_Period = LED_TIM9_PERIOD;
    tb_init.TIM_Prescaler = LED_TIM9_PRESCALER;
    tb_init.TIM_CounterMode = TIM_CounterMode_Up;
    tb_init.TIM_ClockDivision = TIM_CKD_DIV1;
    TIM_TimeBaseInit(TIM9, &tb_init);

    TIM_ClearITPendingBit(TIM9, TIM_IT_Update);
    TIM_ITConfig(TIM9, TIM_IT_Update, ENABLE);
    TIM_Cmd(TIM9, DISABLE);

    NVIC_ClearPendingIRQ(TIM1_BRK_TIM9_IRQn);
    nvic_init.NVIC_IRQChannel = TIM1_BRK_TIM9_IRQn;
    nvic_init.NVIC_IRQChannelPreemptionPriority = 3;
    nvic_init.NVIC_IRQChannelSubPriority = 3;
    nvic_init.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&nvic_init);
}

static void nk60_v1_hc595_init(void)
{
    GPIO_InitTypeDef gpio_init;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);

    gpio_init.GPIO_Pin = HC595_EN_PIN | HC595_CLK_PIN | HC595_DATA_PIN;
    gpio_init.GPIO_Mode = GPIO_Mode_OUT;
    gpio_init.GPIO_OType = GPIO_OType_PP;
    gpio_init.GPIO_Speed = GPIO_Speed_100MHz;
    gpio_init.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOB, &gpio_init);

    nk60_v1_hc595_set_pin(HC595_CLK_PIN, true);
    nk60_v1_hc595_set_pin(HC595_DATA_PIN, true);
    nk60_v1_hc595_set_pin(HC595_EN_PIN, true);

    nk60_v1_hc595_send_word(0U);
}

static ssize_t nk60_v1_led_buf_write(struct device *dev, addr_t pos,
                                     const void *buffer, size_t size)
{
    struct nk60_v1_led *led;
    const uint8_t *buf = buffer;
    uint16_t next_buf[LED_SCAN_ROWS][LED_WIDTH];
    uint32_t row;
    uint32_t col;
    addr_t irq_flags;

    (void)pos;
    if (dev == NULL || dev->priv == NULL || buffer == NULL ||
        size != LED_WIDTH * LED_HEIGHT * DS_COLOR_DATA_MAX) {
        pr_err("data size error\n");
        return -EINVAL;
    }

    led = dev->priv;

    /*
     * 先在局部缓冲区完成 Gamma 1.6 校正、RGB 通道白平衡及分色重组
     * （每按键行展开为 R/G/B 三个物理扫描行），映射为 0..1023 的 10-bit PWM CCR。
     */
    for (row = 0U; row < LED_HEIGHT; row++) {
        for (col = 0U; col < LED_WIDTH; col++) {
            const uint8_t *pixel = &buf[(row * LED_WIDTH + col) * DS_COLOR_DATA_MAX];

            next_buf[row * 3U + 0U][col] = nk60_v1_led_map_channel(pixel[DS_COLOR_R], LED_WB_GAIN_R);
            next_buf[row * 3U + 1U][col] = nk60_v1_led_map_channel(pixel[DS_COLOR_G], LED_WB_GAIN_G);
            next_buf[row * 3U + 2U][col] = nk60_v1_led_map_channel(pixel[DS_COLOR_B], LED_WB_GAIN_B);
        }
    }

    mutex_lock(&led->lock);
    /* 仅在原子替换显示缓冲区时短暂关闭中断，防止 TIM9 扫描过程中出现帧撕裂 */
    irq_flags = disable_irq_save();
    memcpy(s_display_buf, next_buf, sizeof(s_display_buf));
    enable_irq_save(irq_flags);
    mutex_unlock(&led->lock);

    return (ssize_t)size;
}

static int nk60_v1_led_control(struct device *dev, int cmd, void *args)
{
    struct nk60_v1_led *led;
    struct display_dev_info info = { .width = LED_WIDTH, .height = LED_HEIGHT };
    int ret = 0;

    if (dev == NULL || dev->priv == NULL) {
        return -EINVAL;
    }
    led = dev->priv;

    mutex_lock(&led->lock);
    switch (cmd) {
    case LED_CTRL_ENABLE:
        if (!led->enable) {
            addr_t irq_flags = disable_irq_save();

            nk60_v1_pwm_set_led_off();
            nk60_v1_hc595_send_word(0U);
            s_scan_row = 0U;
            TIM_SetCounter(TIM9, 0U);
            TIM_ClearITPendingBit(TIM9, TIM_IT_Update);
            TIM_Cmd(TIM9, ENABLE);
            led->enable = true;
            enable_irq_save(irq_flags);
        }
        break;
    case LED_CTRL_DISABLE:
        {
            addr_t irq_flags = disable_irq_save();

            led->enable = false;
            TIM_Cmd(TIM9, DISABLE);
            TIM_ClearITPendingBit(TIM9, TIM_IT_Update);
            nk60_v1_pwm_set_led_off();
            nk60_v1_hc595_send_word(0U);
            memset(s_display_buf, 0, sizeof(s_display_buf));
            s_scan_row = 0U;
            enable_irq_save(irq_flags);
        }
        break;
    case LED_CTRL_GET_ENABLE_STATUS:
        ret = led->enable ? 1 : 0;
        break;
    case LED_CTRL_REFRESH:
        /* TIM9 更新中断已在后台持续逐行扫描刷新，此处无需额外触发 */
        break;
    case DS_CTRL_GET_DEV_INFO:
        if (args == NULL) {
            ret = -EINVAL;
            break;
        }
        memcpy(args, &info, sizeof(info));
        break;
    default:
        pr_err("unknown cmd: %d\r\n", cmd);
        ret = -EINVAL;
        break;
    }
    mutex_unlock(&led->lock);

    return ret;
}

void TIM1_BRK_TIM9_IRQHandler(void)
{
    if (TIM_GetITStatus(TIM9, TIM_IT_Update) != RESET) {
        uint8_t row = s_scan_row;

        nk60_v1_pwm_set_led_off();
        nk60_v1_hc595_send_bit(row == 0U);
        nk60_v1_pwm_set_led_data(s_display_buf[row]);

        row++;
        if (row >= LED_SCAN_ROWS) {
            row = 0U;
        }
        s_scan_row = row;
        TIM_ClearITPendingBit(TIM9, TIM_IT_Update);
    }
}

static int nk60_v1_led_init(void)
{
    struct nk60_v1_led *led;

    led = kmalloc(sizeof(struct nk60_v1_led), GFP_KERNEL);
    if (led == NULL) {
        pr_err("alloc nk60_v1_led error\n");
        return -ENOMEM;
    }

    mutex_init(&led->lock);
    led->enable = false;
    memset(s_display_buf, 0, sizeof(s_display_buf));
    s_scan_row = 0U;

    nk60_v1_pwm_init();
    nk60_v1_hc595_init();
    nk60_v1_tim9_init();

    device_init(&led->dev);
    led->dev.name = "nk60_v1-led";
    led->dev.ops.write = nk60_v1_led_buf_write;
    led->dev.ops.control = nk60_v1_led_control;
    led->dev.priv = led;
    device_register(&led->dev);

    return 0;
}
task_init(nk60_v1_led_init);
