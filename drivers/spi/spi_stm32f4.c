/** Copyright (C) 2026 Nick Hu */
#include <drivers/stm32_spi.h>

#ifndef CONFIG_STM32_SPI_POLL_TIMEOUT
#define CONFIG_STM32_SPI_POLL_TIMEOUT 1000000U
#endif

int stm32_spi_exchange(struct device *dev, uint8_t tx, uint8_t *rx)
{
    const struct stm32_spi_config *hw = dev->config;
    uint32_t remaining = CONFIG_STM32_SPI_POLL_TIMEOUT;

    while (SPI_I2S_GetFlagStatus(hw->regs, SPI_I2S_FLAG_TXE) == RESET) {
        if (remaining-- == 0) {
            return -ETIMEDOUT;
        }
    }
    SPI_I2S_SendData(hw->regs, tx);
    remaining = CONFIG_STM32_SPI_POLL_TIMEOUT;
    while (SPI_I2S_GetFlagStatus(hw->regs, SPI_I2S_FLAG_RXNE) == RESET) {
        if (remaining-- == 0) {
            return -ETIMEDOUT;
        }
    }
    *rx = (uint8_t)SPI_I2S_ReceiveData(hw->regs);
    return 0;
}

void stm32_spi_select(struct device *dev)
{
    const struct stm32_spi_config *hw = dev->config;
    mutex_lock(&dev->mutex);
    GPIO_ResetBits(hw->cs_gpio, hw->cs_pin);
}

void stm32_spi_deselect(struct device *dev)
{
    const struct stm32_spi_config *hw = dev->config;
    GPIO_SetBits(hw->cs_gpio, hw->cs_pin);
    mutex_unlock(&dev->mutex);
}

static int stm32_spi_transceive(const struct device *dev,
                                const struct spi_config *config,
                                const struct spi_buf *buffers, size_t count)
{
    const struct stm32_spi_config *hw = dev->config;
    struct device *controller = (struct device *)dev;
    size_t i;
    size_t j;
    int ret = 0;

    if (config->chip_select != 0 || config->word_size != 8 ||
        config->mode != hw->mode || config->frequency != hw->frequency) {
        return -EINVAL;
    }
    stm32_spi_select(controller);
    for (i = 0; i < count && ret == 0; i++) {
        const uint8_t *tx = buffers[i].tx;
        uint8_t *rx = buffers[i].rx;
        for (j = 0; j < buffers[i].len; j++) {
            uint8_t value;
            ret = stm32_spi_exchange(controller, tx != NULL ? tx[j] : 0xffU, &value);
            if (ret != 0) {
                break;
            }
            if (rx != NULL) {
                rx[j] = value;
            }
        }
    }
    stm32_spi_deselect(controller);
    return ret;
}

const struct spi_driver_api stm32_spi_driver_api = {
    .transceive = stm32_spi_transceive,
};

int stm32_spi_init(struct device *dev)
{
    const struct stm32_spi_config *hw;
    GPIO_InitTypeDef gpio;
    SPI_InitTypeDef spi;
    uint8_t ignored;
    int ret;

    if (dev == NULL || dev->config == NULL) {
        return -EINVAL;
    }
    hw = dev->config;
    if (hw->regs == NULL || hw->gpio == NULL || hw->cs_gpio == NULL ||
        hw->frequency == 0 || hw->mode != 3) {
        return -EINVAL;
    }
    ret = device_init(dev);
    if (ret != 0) {
        return ret;
    }
    RCC_AHB1PeriphClockCmd(hw->gpio_clock | hw->cs_clock, ENABLE);
    RCC_APB2PeriphClockCmd(hw->spi_clock, ENABLE);
    gpio.GPIO_Pin = hw->pins;
    gpio.GPIO_Mode = GPIO_Mode_AF;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_Speed = GPIO_Speed_100MHz;
    gpio.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(hw->gpio, &gpio);
    GPIO_PinAFConfig(hw->gpio, hw->clock_pin_source, hw->gpio_af);
    GPIO_PinAFConfig(hw->gpio, hw->miso_pin_source, hw->gpio_af);
    GPIO_PinAFConfig(hw->gpio, hw->mosi_pin_source, hw->gpio_af);
    gpio.GPIO_Pin = hw->cs_pin;
    gpio.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_Init(hw->cs_gpio, &gpio);
    GPIO_SetBits(hw->cs_gpio, hw->cs_pin);
    RCC_APB2PeriphResetCmd(hw->spi_clock, ENABLE);
    RCC_APB2PeriphResetCmd(hw->spi_clock, DISABLE);
    spi.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    spi.SPI_Mode = SPI_Mode_Master;
    spi.SPI_DataSize = SPI_DataSize_8b;
    spi.SPI_CPOL = SPI_CPOL_High;
    spi.SPI_CPHA = SPI_CPHA_2Edge;
    spi.SPI_NSS = SPI_NSS_Soft;
    spi.SPI_BaudRatePrescaler = hw->prescaler;
    spi.SPI_FirstBit = SPI_FirstBit_MSB;
    spi.SPI_CRCPolynomial = 7;
    SPI_Init(hw->regs, &spi);
    SPI_Cmd(hw->regs, ENABLE);
    ret = stm32_spi_exchange(dev, 0xffU, &ignored);
    if (ret != 0) {
        return ret;
    }
    ret = device_register(dev);
    if (ret != 0) {
        return ret;
    }
    dev->ready = 1;
    return 0;
}
