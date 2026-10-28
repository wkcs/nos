/** Copyright (C) 2026 Nick Hu */
#include <drivers/stm32_spi.h>
#include <stdio.h>

static int selected, lock_error, count, fail_tx, fail_busy;
static uint8_t sent[8];
static struct device *controller;
int device_init(struct device *dev) { dev->mutex.locked = 0; return 0; }
int device_register(struct device *dev) { (void)dev; return 0; }
void mutex_lock(struct mutex *lock) { if (lock->locked) lock_error = 1; lock->locked = 1; }
void mutex_unlock(struct mutex *lock) { if (!lock->locked) lock_error = 1; lock->locked = 0; }
void RCC_AHB1PeriphClockCmd(uint32_t a, int b) { (void)a; (void)b; }
void RCC_APB2PeriphClockCmd(uint32_t a, int b) { (void)a; (void)b; }
void RCC_APB2PeriphResetCmd(uint32_t a, int b) { (void)a; (void)b; }
void GPIO_Init(GPIO_TypeDef *a, GPIO_InitTypeDef *b) { (void)a; (void)b; }
void GPIO_PinAFConfig(GPIO_TypeDef *a, uint16_t b, uint8_t c) { (void)a; (void)b; (void)c; }
void GPIO_SetBits(GPIO_TypeDef *a, uint16_t b) { (void)a; (void)b; selected = 0; }
void GPIO_ResetBits(GPIO_TypeDef *a, uint16_t b) { (void)a; (void)b; selected = 1; }
void SPI_Init(SPI_TypeDef *a, SPI_InitTypeDef *b) { (void)a; (void)b; }
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
    if (!selected || !controller->mutex.locked) lock_error = 1;
    if (count < (int)sizeof(sent)) sent[count++] = (uint8_t)value;
}
uint16_t SPI_I2S_ReceiveData(SPI_TypeDef *a) { (void)a; return 0x5a; }

int main(void)
{
    SPI_TypeDef spi = {0};
    GPIO_TypeDef gpio = {0};
    const struct stm32_spi_config hw = {
        .regs = &spi, .gpio = &gpio, .gpio_clock = 1, .spi_clock = 2,
        .pins = 0x38, .clock_pin_source = 3, .miso_pin_source = 4,
        .mosi_pin_source = 5, .gpio_af = 5, .cs_gpio = &gpio, .cs_clock = 1,
        .cs_pin = 4, .frequency = 21000000, .prescaler = 4, .mode = 3,
    };
    struct device dev = { .name = "spi1", .config = &hw, .api = &stm32_spi_driver_api };
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
    config.mode = 0;
    if (spi_transceive(&dev, &config, buffers, 2) != -EINVAL || count != 3) {
        fputs("unsupported SPI configuration accepted\n", stderr);
        return 1;
    }
    config.mode = 3;
    fail_tx = 1;
    if (spi_transceive(&dev, &config, buffers, 2) != -ETIMEDOUT ||
        selected || dev.mutex.locked || lock_error) {
        fputs("timeout did not release chip select and lock\n", stderr);
        return 1;
    }
    fail_tx = 0;
    fail_busy = 1;
    if (spi_transceive(&dev, &config, buffers, 2) != -ETIMEDOUT ||
        selected || dev.mutex.locked || lock_error) {
        fputs("busy timeout did not release chip select and lock\n", stderr);
        return 1;
    }
    puts("STM32 SPI checks passed");
    return 0;
}
