/** Copyright (C) 2026 Nick Hu */
#include <drivers/sd_block.h>
#include <drivers/stm32_spi.h>
#include <kernel/init.h>
#include <kernel/printk.h>

/* Wiring follows the original ZJ-V3 SPI SD driver. */
static const struct stm32_spi_cs sd_selects[] = {
    { .gpio = GPIOA, .clock = RCC_APB2Periph_GPIOA, .pin = GPIO_Pin_7, .active_low = 1 },
};

static struct stm32_spi_config sd_bus_config = {
    .regs = SPI2,
    .gpio = GPIOB,
    .gpio_clock = RCC_APB2Periph_GPIOB,
    .spi_clock = RCC_APB1Periph_SPI2,
    .pins = GPIO_Pin_13 | GPIO_Pin_14 | GPIO_Pin_15,
    .miso_pin = GPIO_Pin_14,
    .chip_selects = sd_selects,
    .chip_select_count = 1,
    .apb2 = 0,
};
static struct stm32_spi_data sd_bus_data;
static struct device sd_bus = {
    .name = "zj-spi2",
    .config = &sd_bus_config,
    .data = &sd_bus_data,
    .api = &stm32_spi_driver_api,
};
static const struct sd_spi sd_config = {
    .bus = &sd_bus,
    .config = { .frequency = 18000000U, .chip_select = 0, .word_size = 8, .mode = 0 },
};
static struct sd_spi_card sd_card;
static struct device sd_device = {
    .name = "sd0",
    .config = &sd_config,
    .data = &sd_card,
    .api = &sd_block_driver_api,
};

/* Card initialization sleeps between ACMD41 retries. task_init callbacks run
 * in the core task, after the scheduler has started. */
static int zj_sd_init(void)
{
    RCC_ClocksTypeDef clocks;
    int ret;

    RCC_GetClocksFreq(&clocks);
    sd_bus_config.peripheral_clock = clocks.PCLK1_Frequency;
    ret = stm32_spi_init(&sd_bus);
    if (ret == 0) ret = device_init(&sd_device);
    if (ret == 0) ret = sd_block_init(&sd_device);
    if (ret == 0) ret = device_register(&sd_device);
    if (ret != 0) {
        pr_err("SD initialization failed: %d\r\n", ret);
        return ret;
    }
    sd_device.ready = 1;
    pr_info("SD: %lu sectors registered as sd0\r\n", (unsigned long)sd_card.sector_count);
    return 0;
}

task_init(zj_sd_init);
