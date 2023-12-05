/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include "arch_clk.h"

int clk_enable_all (struct clk_config_t *config)
{
    int err = 0;

    if (config == NULL)
        return 0;

    if (config->clk[CLK_GROUP_APB1] != 0)
        RCC_APB1PeriphClockCmd(config->clk[CLK_GROUP_APB1], ENABLE);

    if (config->clk[CLK_GROUP_APB2] != 0)
        RCC_APB2PeriphClockCmd(config->clk[CLK_GROUP_APB2], ENABLE);

    if (config->clk[CLK_GROUP_AHB1] != 0)
        RCC_AHB1PeriphClockCmd(config->clk[CLK_GROUP_AHB1], ENABLE);

    if (config->clk[CLK_GROUP_AHB2] != 0)
        RCC_AHB2PeriphClockCmd(config->clk[CLK_GROUP_AHB2], ENABLE);

    if (config->clk[CLK_GROUP_AHB3] != 0)
        RCC_AHB3PeriphClockCmd(config->clk[CLK_GROUP_AHB3], ENABLE);

    return err;
}

int clk_disable_all (struct clk_config_t *config)
{
    int err = 0;

    if (config == NULL)
        return 0;

    if (config->clk[CLK_GROUP_APB1] != 0)
        RCC_APB1PeriphClockCmd(config->clk[CLK_GROUP_APB1], DISABLE);

    if (config->clk[CLK_GROUP_APB2] != 0)
        RCC_APB2PeriphClockCmd(config->clk[CLK_GROUP_APB2], DISABLE);

    if (config->clk[CLK_GROUP_AHB1] != 0)
        RCC_AHB1PeriphClockCmd(config->clk[CLK_GROUP_AHB1], DISABLE);

    if (config->clk[CLK_GROUP_AHB2] != 0)
        RCC_AHB2PeriphClockCmd(config->clk[CLK_GROUP_AHB2], DISABLE);

    if (config->clk[CLK_GROUP_AHB2] != 0)
        RCC_AHB2PeriphClockCmd(config->clk[CLK_GROUP_AHB2], DISABLE);

    return err;
}

