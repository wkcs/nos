/** Copyright (C) 2026 Nick Hu */
#include <drivers/stm32_spi.h>
#include <board/nk60_spi.h>

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
    .cs_gpio = GPIOB,
    .cs_clock = RCC_AHB1Periph_GPIOB,
    .cs_pin = GPIO_Pin_2,
    .frequency = 21000000U,
    .prescaler = SPI_BaudRatePrescaler_4,
    .mode = 3,
};

struct device nk60_spi_bus = {
    .name = "nk60-spi1",
    .config = &nk60_spi1_config,
    .api = &stm32_spi_driver_api,
};
