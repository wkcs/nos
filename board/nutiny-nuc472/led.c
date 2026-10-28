/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Author: Nick Hu <huqihan@live.com>
 *
 * PB.10 drives the board I/O LED.
 */
#include <kernel/init.h>
#include <kernel/sleep.h>
#include <kernel/task.h>
#include <stdint.h>

#define REG32(addr) (*(volatile uint32_t *)(addr))
#define SYS_REGLCTL REG32(0x40000100UL)
#define SYS_GPB_MFPH REG32(0x4000003cUL)
#define GPIOB_MODE REG32(0x40004040UL)
#define GPIOB_PIN10 REG32(0x40004868UL)
#define LED_BLINK_INTERVAL_MS 500U

static void nutiny_led_task(void *parameter)
{
    uint32_t level = 0;
    (void)parameter;

    SYS_REGLCTL = 0x59;
    SYS_REGLCTL = 0x16;
    SYS_REGLCTL = 0x88;
    SYS_GPB_MFPH &= ~(0xfU << 8);
    SYS_REGLCTL = 0;

    GPIOB_PIN10 = level;
    GPIOB_MODE = (GPIOB_MODE & ~(3U << 20)) | (1U << 20);

    while (true) {
        msleep(LED_BLINK_INTERVAL_MS);
        level ^= 1U;
        GPIOB_PIN10 = level;
    }
}

static int nutiny_led_init(void)
{
    struct task_struct *task = task_create("board_led", nutiny_led_task,
                                           NULL, 4, 512, 10, NULL);
    if (task == NULL) {
        return -ENOMEM;
    }
    task_ready(task);
    return 0;
}
task_init(nutiny_led_init);
