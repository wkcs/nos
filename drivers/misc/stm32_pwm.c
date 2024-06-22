/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/task.h>
#include <kernel/errno.h>
#include <kernel/sleep.h>
#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/gpio.h>
#include <string.h>
#include <board/board.h>

void pwm_set_val(enum led_ch ch, uint8_t val)
{
    switch (ch) {
        case LED1:
            TIM_SetCompare1(TIM5, val);
            break;
        case LED2:
            TIM_SetCompare2(TIM5, val);
            break;
        case LED3:
            TIM_SetCompare3(TIM5, val);
            break;
        case LED4:
            TIM_SetCompare4(TIM5, val);
            break;
        case LED5:
            TIM_SetCompare1(TIM3, val);
            break;
        case LED6:
            TIM_SetCompare2(TIM3, val);
            break;
        case LED7:
            TIM_SetCompare3(TIM3, val);
            break;
        case LED8:
            TIM_SetCompare4(TIM3, val);
            break;
        case LED9:
            TIM_SetCompare1(TIM4, val);
            break;
        case LED10:
            TIM_SetCompare2(TIM4, val);
            break;
        case LED11:
            TIM_SetCompare3(TIM4, val);
            break;
        case LED12:
            TIM_SetCompare4(TIM4, val);
            break;
        case LED13:
            TIM_SetCompare1(TIM2, val);
            break;
        case LED14:
            TIM_SetCompare3(TIM2, val);
            break;
    }
}

void pwm_set_led_off(void)
{
    TIM_SetCompare1(TIM5, 0);
    TIM_SetCompare2(TIM5, 0);
    TIM_SetCompare3(TIM5, 0);
    TIM_SetCompare4(TIM5, 0);
    TIM_SetCompare1(TIM3, 0);
    TIM_SetCompare2(TIM3, 0);
    TIM_SetCompare3(TIM3, 0);
    TIM_SetCompare4(TIM3, 0);
    TIM_SetCompare1(TIM4, 0);
    TIM_SetCompare2(TIM4, 0);
    TIM_SetCompare3(TIM4, 0);
    TIM_SetCompare4(TIM4, 0);
    TIM_SetCompare1(TIM2, 0);
    TIM_SetCompare3(TIM2, 0);
}

void pwm_set_led_data(uint8_t buf[14])
{
    TIM_SetCompare1(TIM5, buf[0]);
    TIM_SetCompare2(TIM5, buf[1]);
    TIM_SetCompare3(TIM5, buf[2]);
    TIM_SetCompare4(TIM5, buf[3]);
    TIM_SetCompare1(TIM3, buf[4]);
    TIM_SetCompare2(TIM3, buf[5]);
    TIM_SetCompare3(TIM3, buf[6]);
    TIM_SetCompare4(TIM3, buf[7]);
    TIM_SetCompare1(TIM4, buf[8]);
    TIM_SetCompare2(TIM4, buf[9]);
    TIM_SetCompare3(TIM4, buf[10]);
    TIM_SetCompare4(TIM4, buf[11]);
    TIM_SetCompare1(TIM2, buf[12]);
    TIM_SetCompare3(TIM2, buf[13]);
}

int stm32_pwm_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
	TIM_TimeBaseInitTypeDef  TIM_TimeBaseStructure;
	TIM_OCInitTypeDef  TIM_OCInitStructure;

	RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM2, ENABLE);
	RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM3, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM4, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_TIM5, ENABLE);

	RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOA, ENABLE);
    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);

	GPIO_PinAFConfig(GPIOA, GPIO_PinSource5, GPIO_AF_TIM2);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource10, GPIO_AF_TIM2);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource6, GPIO_AF_TIM3);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource7, GPIO_AF_TIM3);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource0, GPIO_AF_TIM3);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource1, GPIO_AF_TIM3);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource6, GPIO_AF_TIM4);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource7, GPIO_AF_TIM4);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource8, GPIO_AF_TIM4);
    GPIO_PinAFConfig(GPIOB, GPIO_PinSource9, GPIO_AF_TIM4);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource0, GPIO_AF_TIM5);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource1, GPIO_AF_TIM5);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource2, GPIO_AF_TIM5);
    GPIO_PinAFConfig(GPIOA, GPIO_PinSource3, GPIO_AF_TIM5);

	GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_2 | GPIO_Pin_3 |
        GPIO_Pin_5 | GPIO_Pin_6 | GPIO_Pin_7;
	GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
	GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
	GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
	GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
	GPIO_Init(GPIOA, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0 | GPIO_Pin_1 | GPIO_Pin_6 | GPIO_Pin_7 |
        GPIO_Pin_8 | GPIO_Pin_9 | GPIO_Pin_10;
    GPIO_Init(GPIOB, &GPIO_InitStructure);

	TIM_TimeBaseStructure.TIM_Prescaler = 4;
	TIM_TimeBaseStructure.TIM_CounterMode = TIM_CounterMode_Up;
	TIM_TimeBaseStructure.TIM_Period = 255;
	TIM_TimeBaseStructure.TIM_ClockDivision = TIM_CKD_DIV1;
	TIM_TimeBaseInit(TIM2,&TIM_TimeBaseStructure);
    TIM_TimeBaseInit(TIM3,&TIM_TimeBaseStructure);
    TIM_TimeBaseInit(TIM4,&TIM_TimeBaseStructure);
    TIM_TimeBaseInit(TIM5,&TIM_TimeBaseStructure);

	//初始化TIM14 Channel1 PWM模式
	TIM_OCInitStructure.TIM_OCMode = TIM_OCMode_PWM1;
 	TIM_OCInitStructure.TIM_OutputState = TIM_OutputState_Enable;
	TIM_OCInitStructure.TIM_OCPolarity = TIM_OCPolarity_Low;
	TIM_OC1Init(TIM2, &TIM_OCInitStructure);
    TIM_OC3Init(TIM2, &TIM_OCInitStructure);

    TIM_OC1Init(TIM3, &TIM_OCInitStructure);
    TIM_OC2Init(TIM3, &TIM_OCInitStructure);
    TIM_OC3Init(TIM3, &TIM_OCInitStructure);
    TIM_OC4Init(TIM3, &TIM_OCInitStructure);

    TIM_OC1Init(TIM4, &TIM_OCInitStructure);
    TIM_OC2Init(TIM4, &TIM_OCInitStructure);
    TIM_OC3Init(TIM4, &TIM_OCInitStructure);
    TIM_OC4Init(TIM4, &TIM_OCInitStructure);

    TIM_OC1Init(TIM5, &TIM_OCInitStructure);
    TIM_OC2Init(TIM5, &TIM_OCInitStructure);
    TIM_OC3Init(TIM5, &TIM_OCInitStructure);
    TIM_OC4Init(TIM5, &TIM_OCInitStructure);

	TIM_OC1PreloadConfig(TIM2, TIM_OCPreload_Enable);
    TIM_OC3PreloadConfig(TIM2, TIM_OCPreload_Enable);

    TIM_OC1PreloadConfig(TIM3, TIM_OCPreload_Enable);
    TIM_OC2PreloadConfig(TIM3, TIM_OCPreload_Enable);
    TIM_OC3PreloadConfig(TIM3, TIM_OCPreload_Enable);
    TIM_OC4PreloadConfig(TIM3, TIM_OCPreload_Enable);

    TIM_OC1PreloadConfig(TIM4, TIM_OCPreload_Enable);
    TIM_OC2PreloadConfig(TIM4, TIM_OCPreload_Enable);
    TIM_OC3PreloadConfig(TIM4, TIM_OCPreload_Enable);
    TIM_OC4PreloadConfig(TIM4, TIM_OCPreload_Enable);

    TIM_OC1PreloadConfig(TIM5, TIM_OCPreload_Enable);
    TIM_OC2PreloadConfig(TIM5, TIM_OCPreload_Enable);
    TIM_OC3PreloadConfig(TIM5, TIM_OCPreload_Enable);
    TIM_OC4PreloadConfig(TIM5, TIM_OCPreload_Enable);

    TIM_ARRPreloadConfig(TIM2,ENABLE);
    TIM_ARRPreloadConfig(TIM3,ENABLE);
    TIM_ARRPreloadConfig(TIM4,ENABLE);
    TIM_ARRPreloadConfig(TIM5,ENABLE);

	TIM_Cmd(TIM2, ENABLE);
    TIM_Cmd(TIM3, ENABLE);
    TIM_Cmd(TIM4, ENABLE);
    TIM_Cmd(TIM5, ENABLE);

    pwm_set_led_off();

    return 0;
}
task_init(stm32_pwm_init);