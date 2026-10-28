/** Copyright (C) 2026 Nick Hu */
#include <drivers/stm32_spi.h>
#include <stdio.h>

static int selected, lock_error, count, fail_tx, fail_busy, expected_pin = 4;
static SPI_InitTypeDef last_init;
static int apb1_clock_calls, apb1_reset_calls;
static uint8_t sent[8];
static struct device *controller;
int device_init(struct device *dev) { dev->mutex.locked = 0; return 0; }
int device_register(struct device *dev) { (void)dev; return 0; }
void mutex_lock(struct mutex *lock) { if (lock->locked) lock_error = 1; lock->locked = 1; }
void mutex_unlock(struct mutex *lock) { if (!lock->locked) lock_error = 1; lock->locked = 0; }
void RCC_AHB1PeriphClockCmd(uint32_t a, int b) { (void)a; (void)b; }
void RCC_APB1PeriphClockCmd(uint32_t a, int b) { (void)a; (void)b; apb1_clock_calls++; }
void RCC_APB2PeriphClockCmd(uint32_t a, int b) { (void)a; (void)b; }
void RCC_APB1PeriphResetCmd(uint32_t a, int b) { (void)a; (void)b; apb1_reset_calls++; }
void RCC_APB2PeriphResetCmd(uint32_t a, int b) { (void)a; (void)b; }
void GPIO_Init(GPIO_TypeDef *a, GPIO_InitTypeDef *b) { (void)a; (void)b; }
void GPIO_PinAFConfig(GPIO_TypeDef *a, uint16_t b, uint8_t c) { (void)a; (void)b; (void)c; }
void GPIO_SetBits(GPIO_TypeDef *a, uint16_t b) { (void)a; (void)b; selected = 0; }
void GPIO_ResetBits(GPIO_TypeDef *a, uint16_t b) { (void)a; selected = b; }
void SPI_Init(SPI_TypeDef *a, SPI_InitTypeDef *b) { (void)a; last_init = *b; }
void SPI_Cmd(SPI_TypeDef *a, int b) { (void)a; (void)b; }
int SPI_I2S_GetFlagStatus(SPI_TypeDef *a, uint16_t b)
{
    (void)a;
    if (b == SPI_I2S_FLAG_BSY) return fail_busy ? 1 : RESET;
    return fail_tx && b == SPI_I2S_FLAG_TXE ? RESET : 1;
}
void SPI_I2S_SendData(SPI_TypeDef *a, uint16_t value)
{
    (void)a;
    if (!controller->ready) return; /* idle clocks during controller init */
    if (selected != expected_pin || !controller->mutex.locked) lock_error = 1;
    if (count < (int)sizeof(sent)) sent[count++] = (uint8_t)value;
}
uint16_t SPI_I2S_ReceiveData(SPI_TypeDef *a) { (void)a; return 0x5a; }

int main(void)
{
    SPI_TypeDef spi = {0};
    GPIO_TypeDef gpio = {0};
    const struct stm32_spi_cs selects[] = {
        { .gpio = &gpio, .clock = 1, .pin = 4, .active_low = 1 },
        { .gpio = &gpio, .clock = 1, .pin = 8, .active_low = 1 },
    };
    const struct stm32_spi_config hw = {
        .regs = &spi, .gpio = &gpio, .gpio_clock = 1, .spi_clock = 2,
        .pins = 0x38, .clock_pin_source = 3, .miso_pin_source = 4,
        .mosi_pin_source = 5, .gpio_af = 5,
        .chip_selects = selects, .chip_select_count = 2, .peripheral_clock = 84000000, .apb2 = 1,
    };
    struct stm32_spi_data state = {0};
    struct device dev = { .name = "spi1", .config = &hw, .data = &state,
                          .api = &stm32_spi_driver_api };
    struct spi_session session = {0};
    struct spi_config config = { .frequency = 21000000, .word_size = 8, .mode = 3 };
    uint8_t command = 0x9f, rx[2] = {0};
    struct spi_buf buffers[] = { { .tx = &command, .len = 1 }, { .rx = rx, .len = 2 } };
    controller = &dev;
    if (stm32_spi_init(&dev) != 0 || !dev.ready ||
        spi_transceive(&dev, &config, buffers, 2) != 0 || selected || lock_error ||
        count != 3 || sent[0] != 0x9f || sent[1] != 0xff || rx[0] != 0x5a || rx[1] != 0x5a) {
        fputs("STM32 SPI transaction failed\n", stderr);
        return 1;
    }
    count = 0;
    if (spi_session_begin(&session, &dev, &config) != 0 || selected != 4 ||
        !dev.mutex.locked || spi_session_transfer(&session, &buffers[0], 1) != 0 ||
        spi_session_transfer(&session, &buffers[1], 1) != 0 || selected != 4 ||
        spi_session_end(&session) != 0 || selected || dev.mutex.locked || count != 3) {
        fputs("held SPI session failed\n", stderr);
        return 1;
    }
    fail_tx = 1;
    if (spi_session_begin(&session, &dev, &config) != 0 ||
        spi_session_transfer(&session, &buffers[0], 1) != -ETIMEDOUT ||
        selected || dev.mutex.locked || session.active) {
        fputs("failed SPI session did not release resources\n", stderr);
        return 1;
    }
    fail_tx = 0;
    config.mode = 0;
    config.chip_select = 1;
    config.frequency = 10000000;
    count = 0;
    expected_pin = 8;
    if (spi_transceive(&dev, &config, buffers, 2) != 0 || selected || lock_error ||
        count != 3 || last_init.SPI_CPOL != SPI_CPOL_Low ||
        last_init.SPI_CPHA != SPI_CPHA_1Edge ||
        last_init.SPI_BaudRatePrescaler != SPI_BaudRatePrescaler_16) {
        fputs("second SPI device configuration failed\n", stderr);
        return 1;
    }
    config.chip_select = 2;
    if (spi_transceive(&dev, &config, buffers, 2) != -EINVAL) {
        fputs("invalid chip select accepted\n", stderr);
        return 1;
    }
    config.chip_select = 0;
    expected_pin = 4;
    config.frequency = 1;
    if (spi_transceive(&dev, &config, buffers, 2) != -EINVAL || selected) {
        fputs("unsupported low frequency accepted\n", stderr);
        return 1;
    }
    config.frequency = 21000000;
    config.mode = 3;
    fail_tx = 1;
    if (spi_transceive(&dev, &config, buffers, 2) != -ETIMEDOUT ||
        selected || dev.mutex.locked || lock_error) {
        fputs("timeout did not release chip select and lock\n", stderr);
        return 1;
    }
    fail_tx = 0;
    if (spi_session_begin(&session, &dev, &config) != 0) return 1;
    fail_busy = 1;
    if (spi_session_end(&session) != -ETIMEDOUT || session.active ||
        selected || dev.mutex.locked) {
        fputs("SPI session end timeout did not release resources\n", stderr);
        return 1;
    }
    if (spi_transceive(&dev, &config, buffers, 2) != -ETIMEDOUT ||
        selected || dev.mutex.locked || lock_error) {
        fputs("busy timeout did not release chip select and lock\n", stderr);
        return 1;
    }
    {
        struct stm32_spi_config apb1_hw = hw;
        struct stm32_spi_data apb1_state = {0};
        struct device apb1_dev = { .name = "spi2", .config = &apb1_hw, .data = &apb1_state,
                                   .api = &stm32_spi_driver_api };
        apb1_hw.apb2 = 0;
        fail_busy = 0;
        controller = &apb1_dev;
        if (stm32_spi_init(&apb1_dev) != 0 || apb1_clock_calls != 1 || apb1_reset_calls != 2) {
            fputs("APB1 controller initialization failed\n", stderr);
            return 1;
        }
    }
    puts("STM32 SPI checks passed");
    return 0;
}
