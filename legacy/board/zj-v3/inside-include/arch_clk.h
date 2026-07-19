/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#ifndef __ARCH_CLK_H__
#define __ARCH_CLK_H__

#include "stm32f10x.h"

#define CLK_GROUP_NUM 3

struct clk_config_t {
    uint32_t clk[CLK_GROUP_NUM];
#define CLK_GROUP_APB1 0
#define CLK_GROUP_APB2 1
#define CLK_GROUP_AHB 2
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
        case CLK_GROUP_AHB:
            RCC_AHBPeriphClockCmd(clk, ENABLE);
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
        case CLK_GROUP_AHB:
            RCC_AHBPeriphClockCmd(clk, DISABLE);
            break;
        default:
            return -1;
    }

    return 0;
}

int clk_enable_all (struct clk_config_t *config);
int clk_disable_all (struct clk_config_t *config);

#endif