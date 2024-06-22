/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/task.h>
#include <kernel/errno.h>
#include <kernel/delay.h>
#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/device.h>
#include <init/init.h>
#include <string.h>
#include <gpio.h>
#include <board.h>
#include "keyboard.h"

#define LUM_MAX 30

static uint8_t display_buf[15][14];
static uint8_t backlight_on_off = BACKLIGHT_ON;
static uint8_t backlight_lum = LUM_MAX;

#define SPI_595_EN  PBout(12)
#define HC_595_CLK  PBout(13)
#define HC_595_DATA PBout(15)

static inline void hc_595_load(void)
{
    SPI_595_EN = 0;
	delay_usec(1);
    SPI_595_EN = 1;
	delay_usec(1);
}

static void HC_595_send(uint16_t data)
{
 	uint8_t j;

  	for (j = 16; j > 0; j--) {
		if(data & 0x8000)
			HC_595_DATA = 1;
		else
			HC_595_DATA = 0;
    	HC_595_CLK = 0;
        delay_usec(1);
    	data <<= 1;
    	HC_595_CLK = 1;
        delay_usec(1);
  	}
	hc_595_load();
}

static void HC_595_send_bit(uint8_t data)
{
	HC_595_DATA = data;
    HC_595_CLK = 0;
    delay_usec(1);
    HC_595_CLK = 1;
    delay_usec(1);
	hc_595_load();
}

void led_write_display_buf(uint8_t *buf)
{
	uint8_t i, m;
	register addr_t level = disable_irq_save();
	memcpy(display_buf, buf, 15 * 14);
	for (i = 0; i < 15; i++) {
		for (m = 0; m < 14; m++)
			display_buf[i][m] = display_buf[i][m] * backlight_lum / LUM_MAX;
	}

	enable_irq_save(level);
}

void led_backlight_on_off(uint8_t on)
{
	if (on == BACKLIGHT_ON) {
		TIM_Cmd(TIM9, ENABLE);
	} else {
		HC_595_send(0);
		TIM_Cmd(TIM9, DISABLE);
	}
}

void led_backlight_set_lum(uint8_t lum)
{
	if (lum > LUM_MAX)
		backlight_lum = LUM_MAX;
	backlight_lum = lum;
}

static void tim7_int_init(void)
{
	TIM_TimeBaseInitTypeDef TIM_TimeBaseInitStructure;
	NVIC_InitTypeDef NVIC_InitStructure;

	RCC_APB2PeriphClockCmd(RCC_APB2Periph_TIM9, ENABLE);

  	TIM_TimeBaseInitStructure.TIM_Period = 83;
	TIM_TimeBaseInitStructure.TIM_Prescaler = 499;
	TIM_TimeBaseInitStructure.TIM_CounterMode = TIM_CounterMode_Up;
	TIM_TimeBaseInitStructure.TIM_ClockDivision = TIM_CKD_DIV1;

	TIM_TimeBaseInit(TIM9, &TIM_TimeBaseInitStructure);

	TIM_ITConfig(TIM9, TIM_IT_Update, ENABLE);
	if (backlight_on_off == BACKLIGHT_ON)
		TIM_Cmd(TIM9, ENABLE);
	else
		TIM_Cmd(TIM9, DISABLE);

	NVIC_InitStructure.NVIC_IRQChannel = TIM1_BRK_TIM9_IRQn;
	NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 0x03;
	NVIC_InitStructure.NVIC_IRQChannelSubPriority = 0x03;
	NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
	NVIC_Init(&NVIC_InitStructure);
}

int spi_74hc595_init(void)
{
    GPIO_InitTypeDef  GPIO_InitStructure;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);

	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_12 | GPIO_Pin_13 | GPIO_Pin_15;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
	GPIO_Init(GPIOB, &GPIO_InitStructure);

	HC_595_CLK = 1;
	HC_595_DATA = 1;
	SPI_595_EN = 1;

	tim7_int_init();
    HC_595_send(0);

	return 0;
}
task_init(spi_74hc595_init);

static uint8_t led_index = 0;
void TIM1_BRK_TIM9_IRQHandler(void)
{
	wk_interrupt_enter();
	if(TIM_GetITStatus(TIM9, TIM_IT_Update) == SET) {
		pwm_set_led_off();
		if (led_index == 0)
			HC_595_send_bit(1);
		else
			HC_595_send_bit(0);
		pwm_set_led_data(display_buf[led_index]);
		led_index++;
		if (led_index >= 15)
			led_index = 0;
	}
	TIM_ClearITPendingBit(TIM9, TIM_IT_Update);
	wk_interrupt_leave();
}