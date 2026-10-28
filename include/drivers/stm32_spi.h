/** Copyright (C) 2026 Nick Hu */
#ifndef __NOS_DRIVERS_STM32_SPI_H__
#define __NOS_DRIVERS_STM32_SPI_H__

#include <drivers/spi.h>
#ifdef CONFIG_SOC_STM32F1
#include <soc/stm32f1xx.h>
#else
#include <soc/stm32f4xx.h>
#endif

struct stm32_spi_cs {
    GPIO_TypeDef *gpio;
    uint32_t clock;
    uint16_t pin;
    uint8_t active_low;
};

struct stm32_spi_config {
    SPI_TypeDef *regs;
    GPIO_TypeDef *gpio;
    uint32_t gpio_clock;
    uint32_t spi_clock;
    uint16_t pins;
    /* F1 configures MISO as an input; the remaining pins are AF outputs. */
    uint16_t miso_pin;
    uint8_t clock_pin_source;
    uint8_t miso_pin_source;
    uint8_t mosi_pin_source;
    uint8_t gpio_af;
    const struct stm32_spi_cs *chip_selects;
    size_t chip_select_count;
    uint32_t peripheral_clock;
    uint8_t apb2;
};

struct stm32_spi_data {
    uint16_t selected;
};

extern const struct spi_driver_api stm32_spi_driver_api;
int stm32_spi_init(struct device *dev);

#endif /* __NOS_DRIVERS_STM32_SPI_H__ */
