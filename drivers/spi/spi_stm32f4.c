/** Copyright (C) 2026 Nick Hu */
#include <drivers/stm32_spi.h>

#ifndef CONFIG_STM32_SPI_POLL_TIMEOUT
#define CONFIG_STM32_SPI_POLL_TIMEOUT 1000000U
#endif

static int stm32_spi_exchange(struct device *dev, uint8_t tx, uint8_t *rx)
{
    const struct stm32_spi_config *hw = dev->config;
    uint32_t remaining = CONFIG_STM32_SPI_POLL_TIMEOUT;

    while (SPI_I2S_GetFlagStatus(hw->regs, SPI_I2S_FLAG_TXE) == RESET) {
        if (remaining-- == 0) return -ETIMEDOUT;
    }
    SPI_I2S_SendData(hw->regs, tx);
    remaining = CONFIG_STM32_SPI_POLL_TIMEOUT;
    while (SPI_I2S_GetFlagStatus(hw->regs, SPI_I2S_FLAG_RXNE) == RESET) {
        if (remaining-- == 0) return -ETIMEDOUT;
    }
    *rx = (uint8_t)SPI_I2S_ReceiveData(hw->regs);
    return 0;
}

static void stm32_spi_cs_set(const struct stm32_spi_cs *cs, int active)
{
    if ((active != 0) == (cs->active_low != 0)) {
        GPIO_ResetBits(cs->gpio, cs->pin);
    } else {
        GPIO_SetBits(cs->gpio, cs->pin);
    }
}

static void stm32_spi_configure(const struct stm32_spi_config *hw,
                                uint8_t mode, uint16_t prescaler)
{
    SPI_InitTypeDef spi;

    SPI_Cmd(hw->regs, DISABLE);
    spi.SPI_Direction = SPI_Direction_2Lines_FullDuplex;
    spi.SPI_Mode = SPI_Mode_Master;
    spi.SPI_DataSize = SPI_DataSize_8b;
    spi.SPI_CPOL = (mode & 2U) ? SPI_CPOL_High : SPI_CPOL_Low;
    spi.SPI_CPHA = (mode & 1U) ? SPI_CPHA_2Edge : SPI_CPHA_1Edge;
    spi.SPI_NSS = SPI_NSS_Soft;
    spi.SPI_BaudRatePrescaler = prescaler;
    spi.SPI_FirstBit = SPI_FirstBit_MSB;
    spi.SPI_CRCPolynomial = 7;
    SPI_Init(hw->regs, &spi);
    SPI_Cmd(hw->regs, ENABLE);
}

static int stm32_spi_prescaler(uint32_t clock, uint32_t maximum, uint16_t *setting)
{
    static const uint16_t values[] = {
        SPI_BaudRatePrescaler_2, SPI_BaudRatePrescaler_4,
        SPI_BaudRatePrescaler_8, SPI_BaudRatePrescaler_16,
        SPI_BaudRatePrescaler_32, SPI_BaudRatePrescaler_64,
        SPI_BaudRatePrescaler_128, SPI_BaudRatePrescaler_256,
    };
    uint32_t divisor = 2;
    size_t i;

    for (i = 0; i < sizeof(values) / sizeof(values[0]); i++, divisor *= 2) {
        uint32_t rate = clock / divisor + (clock % divisor != 0);
        if (rate <= maximum) {
            *setting = values[i];
            return 0;
        }
    }
    return -EINVAL;
}

static int stm32_spi_session_begin(const struct device *dev,
                                   const struct spi_config *config)
{
    const struct stm32_spi_config *hw = dev->config;
    struct stm32_spi_data *data = dev->data;
    struct device *controller = (struct device *)dev;
    uint16_t prescaler;

    if (data == NULL || config->chip_select >= hw->chip_select_count || config->word_size != 8 ||
        stm32_spi_prescaler(hw->peripheral_clock, config->frequency, &prescaler) != 0) {
        return -EINVAL;
    }
    mutex_lock(&controller->mutex);
    stm32_spi_configure(hw, config->mode, prescaler);
    data->selected = config->chip_select;
    stm32_spi_cs_set(&hw->chip_selects[data->selected], 1);
    return 0;
}

static int stm32_spi_session_transfer(const struct device *dev,
                                      const struct spi_buf *buffers, size_t count)
{
    struct device *controller = (struct device *)dev;
    size_t i;
    size_t j;
    int ret;

    for (i = 0; i < count; i++) {
        const uint8_t *tx = buffers[i].tx;
        uint8_t *rx = buffers[i].rx;
        for (j = 0; j < buffers[i].len; j++) {
            uint8_t value;
            ret = stm32_spi_exchange(controller, tx != NULL ? tx[j] : 0xffU, &value);
            if (ret != 0) return ret;
            if (rx != NULL) rx[j] = value;
        }
    }
    return 0;
}

static int stm32_spi_session_end(const struct device *dev)
{
    const struct stm32_spi_config *hw = dev->config;
    struct stm32_spi_data *data = dev->data;
    struct device *controller = (struct device *)dev;
    uint32_t remaining = CONFIG_STM32_SPI_POLL_TIMEOUT;
    int ret = 0;

    while (SPI_I2S_GetFlagStatus(hw->regs, SPI_I2S_FLAG_BSY) != RESET) {
        if (remaining-- == 0) {
            ret = -ETIMEDOUT;
            break;
        }
    }
    stm32_spi_cs_set(&hw->chip_selects[data->selected], 0);
    mutex_unlock(&controller->mutex);
    return ret;
}

static int stm32_spi_transceive(const struct device *dev,
                                const struct spi_config *config,
                                const struct spi_buf *buffers, size_t count)
{
    int ret = stm32_spi_session_begin(dev, config);
    int end_ret;

    if (ret != 0) return ret;
    ret = stm32_spi_session_transfer(dev, buffers, count);
    end_ret = stm32_spi_session_end(dev);
    return ret != 0 ? ret : end_ret;
}

const struct spi_driver_api stm32_spi_driver_api = {
    .transceive = stm32_spi_transceive,
    .session_begin = stm32_spi_session_begin,
    .session_transfer = stm32_spi_session_transfer,
    .session_end = stm32_spi_session_end,
};

int stm32_spi_init(struct device *dev)
{
    const struct stm32_spi_config *hw;
    GPIO_InitTypeDef gpio;
    uint32_t gpio_clocks;
    uint8_t ignored;
    size_t i;
    int ret;

    if (dev == NULL || dev->config == NULL || dev->data == NULL) return -EINVAL;
    hw = dev->config;
    if (hw->regs == NULL || hw->gpio == NULL || hw->chip_selects == NULL ||
        hw->chip_select_count == 0 || hw->peripheral_clock == 0) return -EINVAL;
    gpio_clocks = hw->gpio_clock;
    for (i = 0; i < hw->chip_select_count; i++) {
        if (hw->chip_selects[i].gpio == NULL || hw->chip_selects[i].pin == 0) return -EINVAL;
        gpio_clocks |= hw->chip_selects[i].clock;
    }
    ret = device_init(dev);
    if (ret != 0) return ret;
    RCC_AHB1PeriphClockCmd(gpio_clocks, ENABLE);
    if (hw->apb2) {
        RCC_APB2PeriphClockCmd(hw->spi_clock, ENABLE);
    } else {
        RCC_APB1PeriphClockCmd(hw->spi_clock, ENABLE);
    }
    gpio.GPIO_Pin = hw->pins;
    gpio.GPIO_Mode = GPIO_Mode_AF;
    gpio.GPIO_OType = GPIO_OType_PP;
    gpio.GPIO_Speed = GPIO_Speed_100MHz;
    gpio.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(hw->gpio, &gpio);
    GPIO_PinAFConfig(hw->gpio, hw->clock_pin_source, hw->gpio_af);
    GPIO_PinAFConfig(hw->gpio, hw->miso_pin_source, hw->gpio_af);
    GPIO_PinAFConfig(hw->gpio, hw->mosi_pin_source, hw->gpio_af);
    for (i = 0; i < hw->chip_select_count; i++) {
        gpio.GPIO_Pin = hw->chip_selects[i].pin;
        gpio.GPIO_Mode = GPIO_Mode_OUT;
        GPIO_Init(hw->chip_selects[i].gpio, &gpio);
        stm32_spi_cs_set(&hw->chip_selects[i], 0);
    }
    if (hw->apb2) {
        RCC_APB2PeriphResetCmd(hw->spi_clock, ENABLE);
        RCC_APB2PeriphResetCmd(hw->spi_clock, DISABLE);
    } else {
        RCC_APB1PeriphResetCmd(hw->spi_clock, ENABLE);
        RCC_APB1PeriphResetCmd(hw->spi_clock, DISABLE);
    }
    stm32_spi_configure(hw, 3, SPI_BaudRatePrescaler_256);
    ret = stm32_spi_exchange(dev, 0xffU, &ignored);
    if (ret != 0) return ret;
    ret = device_register(dev);
    if (ret != 0) return ret;
    dev->ready = 1;
    return 0;
}
