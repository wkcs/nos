/** Copyright (C) 2026 Nick Hu */
#ifndef __NOS_DRIVERS_STM32_SPI_H__
#define __NOS_DRIVERS_STM32_SPI_H__

#include <drivers/spi.h>
#include <soc/stm32f4xx.h>

struct stm32_spi_config {
    SPI_TypeDef *regs;
    GPIO_TypeDef *gpio;
    uint32_t gpio_clock;
    uint32_t spi_clock;
    uint16_t pins;
    uint8_t clock_pin_source;
    uint8_t miso_pin_source;
    uint8_t mosi_pin_source;
    uint8_t gpio_af;
    GPIO_TypeDef *cs_gpio;
    uint32_t cs_clock;
    uint16_t cs_pin;
    uint32_t frequency;
    uint16_t prescaler;
    uint8_t mode;
};

extern const struct spi_driver_api stm32_spi_driver_api;
int stm32_spi_init(struct device *dev);
void stm32_spi_select(struct device *dev);
void stm32_spi_deselect(struct device *dev);
int stm32_spi_exchange(struct device *dev, uint8_t tx, uint8_t *rx);

#endif /* __NOS_DRIVERS_STM32_SPI_H__ */
