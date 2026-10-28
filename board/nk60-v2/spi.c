/** Copyright (C) 2026 Nick Hu */
#include <drivers/stm32_spi.h>
#include <board/nk60_spi.h>

static const struct stm32_spi_cs nk60_spi1_selects[] = {
    { .gpio = GPIOB, .clock = RCC_AHB1Periph_GPIOB, .pin = GPIO_Pin_2, .active_low = 1 },
};

static const struct stm32_spi_config nk60_spi1_config = {
    .regs = SPI1,
    .gpio = GPIOB,
    .gpio_clock = RCC_AHB1Periph_GPIOB,
    .spi_clock = RCC_APB2Periph_SPI1,
    .pins = GPIO_Pin_3 | GPIO_Pin_4 | GPIO_Pin_5,
    .clock_pin_source = GPIO_PinSource3,
    .miso_pin_source = GPIO_PinSource4,
    .mosi_pin_source = GPIO_PinSource5,
    .gpio_af = GPIO_AF_SPI1,
    .chip_selects = nk60_spi1_selects,
    .chip_select_count = sizeof(nk60_spi1_selects) / sizeof(nk60_spi1_selects[0]),
    .peripheral_clock = 84000000U,
    .apb2 = 1,
};

struct device nk60_spi_bus = {
    .name = "nk60-spi1",
    .config = &nk60_spi1_config,
    .api = &stm32_spi_driver_api,
};
