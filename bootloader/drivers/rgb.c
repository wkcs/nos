/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60 Bootloader: RGB 状态指示灯驱动实现（NK60-V1 74HC595 + 14 路 PWM / NK60-V2 WS2812）
 */

#include "rgb.h"
#include "clock.h"
#include <stm32f4xx.h>

#if defined(BOARD_NK60_V1)

#define NK60_V1_LED_SCAN_ROWS       15U

static volatile uint8_t s_rgb_color[3];
static volatile uint8_t s_scan_line;

static inline void hc595_delay(void)
{
    /* @84MHz 约 95ns，满足 74HC595 在 3.3V 下的最小脉宽与建立时间 */
    __asm__ volatile (
        "nop\n\tnop\n\tnop\n\tnop\n\t"
        "nop\n\tnop\n\tnop\n\tnop\n\t"
    );
}

static void hc595_send_word(uint16_t data)
{
    uint32_t i;

    for (i = 0U; i < 16U; i++) {
        /* MSB 先出，最后移入的 bit 0 落在第 1 级 74HC595 的 QA（Row 0） */
        GPIOB->BSRR = ((data & 0x8000U) != 0U) ? (1UL << 15U) : (1UL << (15U + 16U));
        GPIOB->BSRR = (1UL << (13U + 16U));
        hc595_delay();
        data <<= 1U;
        GPIOB->BSRR = (1UL << 13U);
        hc595_delay();
    }

    /* EN (PB12) 上升沿锁存输出 */
    GPIOB->BSRR = (1UL << (12U + 16U));
    hc595_delay();
    GPIOB->BSRR = (1UL << 12U);
    hc595_delay();
}

static void pwm_set_all_cols(uint8_t duty)
{
    TIM5->CCR1 = duty;
    TIM5->CCR2 = duty;
    TIM5->CCR3 = duty;
    TIM5->CCR4 = duty;

    TIM3->CCR1 = duty;
    TIM3->CCR2 = duty;
    TIM3->CCR3 = duty;
    TIM3->CCR4 = duty;

    TIM4->CCR1 = duty;
    TIM4->CCR2 = duty;
    TIM4->CCR3 = duty;
    TIM4->CCR4 = duty;

    TIM2->CCR1 = duty;
    TIM2->CCR3 = duty;
}

static void pwm_init_timer(TIM_TypeDef *tim, uint32_t ccmr1, uint32_t ccmr2, uint32_t ccer)
{
    tim->CR1 = 0U;
    tim->PSC = 8U;
    tim->ARR = 255U;
    tim->CNT = 0U;
    tim->CCMR1 = ccmr1;
    tim->CCMR2 = ccmr2;
    tim->CCR1 = 0U;
    tim->CCR2 = 0U;
    tim->CCR3 = 0U;
    tim->CCR4 = 0U;
    tim->CCER = ccer;
    tim->EGR = TIM_EGR_UG;
    tim->CR1 = TIM_CR1_ARPE | TIM_CR1_CEN;
}

void rgb_init(void)
{
    const uint32_t pb_hc_mask = (3UL << 24U) | (3UL << 26U) | (3UL << 30U);
    const uint32_t pb_hc_out = (1UL << 24U) | (1UL << 26U) | (1UL << 30U);

    s_rgb_color[0] = 0U;
    s_rgb_color[1] = 0U;
    s_rgb_color[2] = 0U;
    s_scan_line = 0U;

    /* 使能 GPIOA/B 与 TIM2/3/4/5/9 时钟 */
    RCC->AHB1ENR |= (RCC_AHB1ENR_GPIOAEN | RCC_AHB1ENR_GPIOBEN);
    RCC->APB1ENR |= (RCC_APB1ENR_TIM2EN | RCC_APB1ENR_TIM3EN |
                     RCC_APB1ENR_TIM4EN | RCC_APB1ENR_TIM5EN);
    RCC->APB2ENR |= RCC_APB2ENR_TIM9EN;

    /* 74HC595 控制引脚 PB12(EN), PB13(CLK), PB15(DATA) 推挽输出，避免触及 PB14 */
    GPIOB->MODER = (GPIOB->MODER & ~pb_hc_mask) | pb_hc_out;
    GPIOB->OTYPER &= ~((1UL << 12U) | (1UL << 13U) | (1UL << 15U));
    GPIOB->OSPEEDR |= pb_hc_mask;
    GPIOB->PUPDR = (GPIOB->PUPDR & ~pb_hc_mask) | pb_hc_out;
    GPIOB->BSRR = (1UL << 12U) | (1UL << 13U) | (1UL << 15U);
    hc595_send_word(0U);

    /*
     * 配置 14 路 PWM 列引脚复用功能：
     * - GPIOA: PA0..PA3 (AF2 TIM5), PA5 (AF1 TIM2), PA6..PA7 (AF2 TIM3)，保留 PA4
     * - GPIOB: PB0..PB1 (AF2 TIM3), PB6..PB9 (AF2 TIM4), PB10 (AF1 TIM2)
     */
    GPIOA->MODER = (GPIOA->MODER & ~0x0000FCFFUL) | 0x0000A8AAUL;
    GPIOA->OTYPER &= ~0x000000EFUL;
    GPIOA->OSPEEDR |= 0x0000FCFFUL;
    GPIOA->AFR[0] = (GPIOA->AFR[0] & ~0xFFF0FFFFUL) | 0x22102222UL;

    GPIOB->MODER = (GPIOB->MODER & ~0x003FF00FUL) | 0x002AA00AUL;
    GPIOB->OTYPER &= ~0x000007C3UL;
    GPIOB->OSPEEDR |= 0x003FF00FUL;
    GPIOB->AFR[0] = (GPIOB->AFR[0] & ~0xFF0000FFUL) | 0x22000022UL;
    GPIOB->AFR[1] = (GPIOB->AFR[1] & ~0x00000FFFUL) | 0x00000122UL;

    /* 84MHz / (8+1) / 256 ≈ 36.46kHz PWM，低电平有效 (CCxP=1, CCxE=1) */
    pwm_init_timer(TIM5, 0x6868U, 0x6868U, 0x3333U);
    pwm_init_timer(TIM3, 0x6868U, 0x6868U, 0x3333U);
    pwm_init_timer(TIM4, 0x6868U, 0x6868U, 0x3333U);
    pwm_init_timer(TIM2, 0x0068U, 0x0068U, 0x0303U);
    pwm_set_all_cols(0U);

    /* TIM9 行扫描定时器：84MHz / (499+1) / (83+1) = 2 kHz */
    TIM9->CR1 = 0U;
    TIM9->PSC = 499U;
    TIM9->ARR = 83U;
    TIM9->CNT = 0U;
    TIM9->SR = 0U;
    TIM9->DIER = TIM_DIER_UIE;
    NVIC_ClearPendingIRQ(TIM1_BRK_TIM9_IRQn);
    NVIC_SetPriority(TIM1_BRK_TIM9_IRQn, 2U);
    NVIC_EnableIRQ(TIM1_BRK_TIM9_IRQn);
    TIM9->CR1 = TIM_CR1_CEN;
}

void rgb_set_solid(uint8_t r, uint8_t g, uint8_t b)
{
    uint8_t first_active_line = (r != 0U) ? 0U : ((g != 0U) ? 1U : ((b != 0U) ? 2U : 0U));

    NVIC_DisableIRQ(TIM1_BRK_TIM9_IRQn);

    s_rgb_color[0] = r;
    s_rgb_color[1] = g;
    s_rgb_color[2] = b;

    /* 立即锁存并点亮首条有效通道行，确保擦除内部 Flash 阻塞期间指示灯保持常亮 */
    pwm_set_all_cols(0U);
    hc595_send_word((uint16_t)(1U << first_active_line));
    pwm_set_all_cols(s_rgb_color[first_active_line]);
    s_scan_line = (uint8_t)(first_active_line + 1U);

    TIM9->SR = 0U;
    NVIC_ClearPendingIRQ(TIM1_BRK_TIM9_IRQn);
    NVIC_EnableIRQ(TIM1_BRK_TIM9_IRQn);
}

void TIM1_BRK_TIM9_IRQHandler(void)
{
    if ((TIM9->SR & TIM_SR_UIF) != 0UL) {
        uint8_t line = s_scan_line;

        TIM9->SR = (uint16_t)~TIM_SR_UIF;
        pwm_set_all_cols(0U);
        hc595_send_word((uint16_t)(1U << line));
        pwm_set_all_cols(s_rgb_color[line % 3U]);

        line++;
        if (line >= NK60_V1_LED_SCAN_ROWS) {
            line = 0U;
        }
        s_scan_line = line;
    }
}

#else

void rgb_init(void)
{
    /* 使能 GPIOB, GPIOC 与 SPI2 时钟 */
    RCC->AHB1ENR |= (RCC_AHB1ENR_GPIOBEN | RCC_AHB1ENR_GPIOCEN);
    RCC->APB1ENR |= RCC_APB1ENR_SPI2EN;

    /* PB0 配置为推挽输出 (灯珠电源使能/片选控制) */
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << 0U)) | (1UL << 0U);
    GPIOB->BSRR = (1UL << 16U); /* 低电平复位 */

    /* PC3 配置为 SPI2_MOSI (AF5) */
    GPIOC->MODER = (GPIOC->MODER & ~(3UL << 6U)) | (2UL << 6U);
    GPIOC->OSPEEDR |= (3UL << 6U);
    GPIOC->AFR[0] = (GPIOC->AFR[0] & ~(0xFUL << 12U)) | (5UL << 12U);

    /* SPI2 主机模式，双线只发，波特率分频 = 8 (42MHz / 8 = 5.25MHz), 对应 WS2812 码元时序 */
    SPI2->CR1 = SPI_CR1_BIDIMODE | SPI_CR1_BIDIOE | SPI_CR1_SSM | SPI_CR1_SSI |
                (2UL << 3U) | SPI_CR1_MSTR | SPI_CR1_CPHA | SPI_CR1_CPOL | SPI_CR1_SPE;
}

static void rgb_spi_send_byte(uint8_t val)
{
    int i;
    for (i = 0; i < 8; i++) {
        uint8_t wire = ((val << i) & 0x80U) ? 0xF0U : 0xC0U;
        while ((SPI2->SR & SPI_SR_TXE) == 0UL) {
        }
        SPI2->DR = wire;
    }
}

void rgb_set_solid(uint8_t r, uint8_t g, uint8_t b)
{
    int i;
    GPIOB->BSRR = (1UL << 0U);
    delay_us(200U);
    for (i = 0; i < 61; i++) {
        rgb_spi_send_byte(g);
        rgb_spi_send_byte(r);
        rgb_spi_send_byte(b);
    }
    while ((SPI2->SR & SPI_SR_BSY) != 0UL) {
    }
    delay_us(100U);
}

#endif /* BOARD_NK60_V1 */

