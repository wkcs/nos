/** Copyright (C) 2026 Nick Hu */
#include <drivers/sd_block.h>
#include <drivers/spi_dt.h>
#include <drivers/stm32_spi.h>
#include <kernel/init.h>
#include <kernel/printk.h>
#include <nos/driver.h>

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
static struct sd_spi sd_config = {
    .bus = &sd_bus,
    .config = { .frequency = 18000000U, .chip_select = 0, .word_size = 8, .mode = 0 },
};

static int zj_sd_probe(struct device *dev, const struct nos_dt_node *node,
                       const void *match_data)
{
    (void)node;
    (void)match_data;
    return sd_block_init(dev);
}

static const struct nos_dt_match sd_matches[] = {
    { .compatible = "mmc-spi-slot" },
};
static const struct nos_dt_driver sd_driver = {
    .name = "sd-spi",
    .matches = sd_matches,
    .match_count = 1,
    .probe = zj_sd_probe,
};

static int zj_sd_resolve(const struct nos_dt_node **out)
{
    const struct nos_dt_node *chosen = nos_dt_find_path("/chosen");
    const struct nos_dt_node *node;
    struct spi_dt_resources resources;
    uint32_t phandle, bus_address, gpio_address;

    if (nos_dt_read_u32(chosen, "nos,sd", 0, &phandle) != 0) return -EINVAL;
    node = nos_dt_find_phandle(phandle);
    if (!nos_dt_is_compatible(node, "mmc-spi-slot") ||
        spi_dt_resolve(node, &resources) != 0 ||
        !nos_dt_is_compatible(resources.bus, "st,stm32f1-spi") ||
        !nos_dt_is_compatible(resources.gpio, "st,stm32f1-gpio") ||
        nos_dt_read_u32(resources.bus, "reg", 0, &bus_address) != 0 ||
        nos_dt_read_u32(resources.gpio, "reg", 0, &gpio_address) != 0 ||
        bus_address != (uint32_t)(uintptr_t)SPI2 ||
        gpio_address != (uint32_t)(uintptr_t)GPIOA ||
        resources.gpio_pin != 7 || resources.gpio_flags != 1 ||
        resources.config.chip_select != 0 || resources.config.mode != 0 ||
        resources.config.frequency > 18000000U) return -EINVAL;
    sd_config.config = resources.config;
    *out = node;
    return 0;
}
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
    const struct nos_dt_node *node;
    int ret;

    ret = zj_sd_resolve(&node);
    if (ret != 0) return ret;
    RCC_GetClocksFreq(&clocks);
    sd_bus_config.peripheral_clock = clocks.PCLK1_Frequency;
    ret = stm32_spi_init(&sd_bus);
    if (ret == 0) ret = device_init(&sd_device);
    if (ret == 0) ret = nos_dt_device_bind(&sd_device, node, &sd_driver);
    if (ret == 0) ret = device_register(&sd_device);
    if (ret != 0) {
        pr_err("SD initialization failed: %d\n", ret);
        return ret;
    }
    sd_device.ready = 1;
    pr_info("SD: %lu sectors registered as sd0\n", (unsigned long)sd_card.sector_count);
    return 0;
}

task_init(zj_sd_init);
