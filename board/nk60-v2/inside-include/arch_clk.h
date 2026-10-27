/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ARCH_CLK_H__
#define __ARCH_CLK_H__

#include "stm32f4xx.h"

#define CLK_GROUP_NUM 5

struct clk_config_t {
    uint32_t clk[CLK_GROUP_NUM];
#define CLK_GROUP_APB1 0
#define CLK_GROUP_APB2 1
#define CLK_GROUP_AHB1 2
#define CLK_GROUP_AHB2 3
#define CLK_GROUP_AHB3 4
};

inline int clk_enable (uint32_t clk, uint8_t group)
{
    switch (group) {
        case CLK_GROUP_APB1:
            RCC_APB1PeriphClockCmd(clk, ENABLE);
            break;
        case CLK_GROUP_APB2:
            RCC_APB2PeriphClockCmd(clk, ENABLE);
            break;
        case CLK_GROUP_AHB1:
            RCC_AHB1PeriphClockCmd(clk, ENABLE);
            break;
        case CLK_GROUP_AHB2:
            RCC_AHB2PeriphClockCmd(clk, ENABLE);
            break;
        case CLK_GROUP_AHB3:
            RCC_AHB3PeriphClockCmd(clk, ENABLE);
            break;
        default:
            return -1;
    }

    return 0;
}

inline int clk_disable (uint32_t clk, uint8_t group)
{

    switch (group) {
        case CLK_GROUP_APB1:
            RCC_APB1PeriphClockCmd(clk, DISABLE);
            break;
        case CLK_GROUP_APB2:
            RCC_APB2PeriphClockCmd(clk, DISABLE);
            break;
        case CLK_GROUP_AHB1:
            RCC_AHB1PeriphClockCmd(clk, DISABLE);
            break;
        case CLK_GROUP_AHB2:
            RCC_AHB2PeriphClockCmd(clk, DISABLE);
            break;
        case CLK_GROUP_AHB3:
            RCC_AHB3PeriphClockCmd(clk, DISABLE);
            break;
        default:
            return -1;
    }

    return 0;
}

int clk_enable_all (struct clk_config_t *config);
int clk_disable_all (struct clk_config_t *config);

#endif