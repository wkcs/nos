/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * PB.10 驱动 NuTiny-NUC472 板载用户指示 LED (复用 gpio_nuc472 驱动)
 */
#include <stdint.h>
#include <stdbool.h>
#include <kernel/init.h>
#include <kernel/timer.h>
#include <soc/nuc472_soc.h>
#include <drivers/gpio_nuc472.h>

#define LED_BLINK_INTERVAL_MS 500U

volatile bool g_nutiny_led_blinking = true;
static struct timer s_nutiny_led_timer;

int board_led_set(bool on)
{
    g_nutiny_led_blinking = false;
    nuc472_sys_unlock();
    NUC472_SYS->GPB_MFPH &= ~(0xFU << 8);
    nuc472_sys_lock();
    nuc472_gpio_config(NUC472_PORT_B, 10, NUC472_GPIO_MODE_OUTPUT);
    /* NuTiny-NUC472 板载 LED 为低电平点亮 (Active-Low) */
    nuc472_gpio_write(NUC472_PORT_B, 10, on ? 0 : 1);
    return 0;
}

static void nutiny_led_timer_cb(void *parameter)
{
    (void)parameter;
    if (g_nutiny_led_blinking) {
        nuc472_gpio_toggle(NUC472_PORT_B, 10);
    }
}

static int nutiny_led_init(void)
{
    /* 配置 PB.10 为 GPIO 功能 (清空 GPB_MFPH 引脚复用位) */
    nuc472_sys_unlock();
    NUC472_SYS->GPB_MFPH &= ~(0xFU << 8);
    nuc472_sys_lock();

    /* 使用 NUC472 GPIO 驱动配置 PB.10 为推挽输出，默认高电平熄灭 (Active-Low) */
    nuc472_gpio_config(NUC472_PORT_B, 10, NUC472_GPIO_MODE_OUTPUT);
    nuc472_gpio_write(NUC472_PORT_B, 10, 1);

    timer_init(&s_nutiny_led_timer, "board_led", nutiny_led_timer_cb, NULL);
    timer_start_periodic(&s_nutiny_led_timer, LED_BLINK_INTERVAL_MS / CONFIG_SYS_TICK_MS);
    return 0;
}
task_init(nutiny_led_init);
