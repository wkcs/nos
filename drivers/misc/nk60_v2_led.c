/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[LED]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/task.h>
#include <kernel/errno.h>
#include <kernel/sleep.h>
#include <kernel/cpu.h>
#include <kernel/irq.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/gpio.h>
#include <kernel/spinlock.h>
#include <kernel/mm.h>
#include <string.h>
#include <board/board.h>
#include <kernel/sem.h>
#include <kernel/mutex.h>

#include <display/led.h>
#include <display/display.h>

#define LED_VCC_EN PBout(0)

#define LED_DATA_0 0xc0
#define LED_DATA_1 0xf0
#define LED_COLOR_BIT_WIDTH 8

#define LED_NUM 61
#define LED_BUF_SZIE (LED_NUM * COLOR_MAX * LED_COLOR_BIT_WIDTH)

#define LED_WIDTH 14
#define LED_HEIGHT 5

static uint8_t g_led_buf[LED_BUF_SZIE];

struct nk60_led {
    struct mutex lock;
    struct device dev;
    sem_t sem;
    uint8_t *buf;

    bool enable;
};
struct nk60_led *g_led;

static unsigned int g_nk60_led_buf_index[LED_WIDTH * LED_HEIGHT] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 40,
    41, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 52,
    53, 54, 55, 56, 56, 56, 56, 56, 56, 56, 57, 58, 59, 60
};

static void nk60_v2_led_dma_start(void)
{
    DMA_Cmd(DMA1_Stream4, DISABLE);
    DMA_SetCurrDataCounter(DMA1_Stream4, LED_BUF_SZIE);
    DMA_Cmd(DMA1_Stream4, ENABLE);
}

static void nk60_v2_led_dma_stop(void)
{
    DMA_Cmd(DMA1_Stream4, DISABLE);
}

static void nk60_v2_led_write_single_color_buf(uint8_t data, uint8_t color_type, int index)
{
    int i;
    uint8_t tmp;

    for (i = 0; i < 8; i++) {
        tmp = (0x80 & (data << i)) ? LED_DATA_1 : LED_DATA_0;
        g_led_buf[index * (COLOR_MAX * LED_COLOR_BIT_WIDTH) + (color_type * LED_COLOR_BIT_WIDTH) + i] = tmp;
    }
}

static void nk60_v2_led_write_buf(uint8_t r, uint8_t g, uint8_t b, int index)
{
    int i;
    uint8_t data;

    if (unlikely(index >= LED_NUM))
        return;

    /* green */
    for (i = 0; i < 8; i++) {
        data = (0x80 & (g << i)) ? LED_DATA_1 : LED_DATA_0;
        g_led_buf[index * (COLOR_MAX * LED_COLOR_BIT_WIDTH) + i + LED_COLOR_BIT_WIDTH * COLOR_G] = data;
    }

    /* red */
    for (i = 0; i < 8; i++) {
        data = (0x80 & (r << i)) ? LED_DATA_1 : LED_DATA_0;
        g_led_buf[index * (COLOR_MAX * LED_COLOR_BIT_WIDTH) + i + LED_COLOR_BIT_WIDTH * COLOR_R] = data;
    }

    /* blue */
    for (i = 0; i < 8; i++) {
        data = (0x80 & (b << i)) ? LED_DATA_1 : LED_DATA_0;
        g_led_buf[index * (COLOR_MAX * LED_COLOR_BIT_WIDTH) + i + LED_COLOR_BIT_WIDTH * COLOR_B] = data;
    }
}

static ssize_t nk60_v2_led_buf_write(struct device *dev, addr_t pos, const void *buffer, size_t size)
{
    struct nk60_led *led = dev->priv;
    const uint8_t *buf = buffer;
    int i, index;

    if (size != LED_WIDTH * LED_HEIGHT * DS_COLOR_DATA_MAX) {
        pr_err("data size error\r\n");
        return -EINVAL;
    }

    mutex_lock(&led->lock);
    for (i = 0; i < (LED_WIDTH * LED_HEIGHT * DS_COLOR_DATA_MAX); i += DS_COLOR_DATA_MAX) {
        index = g_nk60_led_buf_index[i / DS_COLOR_DATA_MAX];
        nk60_v2_led_write_buf(buf[i + DS_COLOR_R], buf[i + DS_COLOR_G], buf[i + DS_COLOR_B], index);
    }
    mutex_unlock(&led->lock);

    return size;
}

static void nk60_v2_led_buf_init(struct nk60_led *led)
{
    mutex_lock(&led->lock);
    memset(led->buf, LED_DATA_0, LED_BUF_SZIE);
    mutex_unlock(&led->lock);
}

static int nk60_v2_led_control(struct device *dev, int cmd, void *args)
{
    struct nk60_led *led = dev->priv;
    struct display_dev_info info = {.width = 14, .height = 5};

    switch (cmd) {
    case LED_CTRL_ENABLE:
        LED_VCC_EN = 1;
        led->enable = true;
        break;
    case LED_CTRL_DISABLE:
        led->enable = false;
        LED_VCC_EN = 0;
        nk60_v2_led_buf_init(led);
        break;
    case LED_CTRL_GET_ENABLE_STATUS:
        return led->enable;
    case LED_CTRL_REFRESH:
        mutex_lock(&led->lock);
        nk60_v2_led_dma_start();
        sem_get(&led->sem);
        mutex_unlock(&led->lock);
        break;
    case DS_CTRL_GET_DEV_INFO:
        memcpy(args, &info, sizeof(struct display_dev_info));
        break;
    default:
        pr_err("unknown cmd: %d\r\n", cmd);
        return -EINVAL;
    }

    return 0;
}

static void nk60_v2_led_spi_set_speed(uint8_t SPI_BaudRatePrescaler)
{
    assert_param(IS_SPI_BAUDRATE_PRESCALER(SPI_BaudRatePrescaler));
    SPI2->CR1 &= 0XFFC7;
    SPI2->CR1 |= SPI_BaudRatePrescaler;
    SPI_Cmd(SPI2, ENABLE);
}

static int nk60_v2_led_spi_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    SPI_InitTypeDef SPI_InitStructure;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOC, ENABLE);
    RCC_APB1PeriphClockCmd(RCC_APB1Periph_SPI2, ENABLE);

    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_3;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_DOWN;
    GPIO_Init(GPIOC, &GPIO_InitStructure);

    GPIO_PinAFConfig(GPIOC, GPIO_PinSource3, GPIO_AF_SPI2);

    RCC_APB1PeriphResetCmd(RCC_APB1Periph_SPI2, ENABLE);
    RCC_APB1PeriphResetCmd(RCC_APB1Periph_SPI2, DISABLE);

    SPI_InitStructure.SPI_Direction = SPI_Direction_1Line_Tx;
    SPI_InitStructure.SPI_Mode = SPI_Mode_Master;
    SPI_InitStructure.SPI_DataSize = SPI_DataSize_8b;
    SPI_InitStructure.SPI_CPOL = SPI_CPOL_High;
    SPI_InitStructure.SPI_CPHA = SPI_CPHA_2Edge;
    SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;
    SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_8;
    SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;
    SPI_InitStructure.SPI_CRCPolynomial = 7;
    SPI_Init(SPI2, &SPI_InitStructure);

    SPI_Cmd(SPI2, ENABLE);
    SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Tx, ENABLE);

    return 0;
}

static int nk60_v2_led_gpio_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_GPIOB, ENABLE);
    GPIO_InitStructure.GPIO_Pin = GPIO_Pin_0;
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_OUT;
    GPIO_InitStructure.GPIO_OType = GPIO_OType_PP;
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_100MHz;
    GPIO_InitStructure.GPIO_PuPd = GPIO_PuPd_UP;
    GPIO_Init(GPIOB, &GPIO_InitStructure);
    LED_VCC_EN = 0;

    return 0;
}

static int nk60_v2_led_dma_init(void)
{
    DMA_InitTypeDef init_type;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA1, ENABLE);
    DMA_DeInit(DMA1_Stream4);

    init_type.DMA_Channel = DMA_Channel_0;
    init_type.DMA_PeripheralBaseAddr = (uint32_t)&SPI2->DR;
    init_type.DMA_Memory0BaseAddr = (uint32_t)g_led_buf;
    init_type.DMA_DIR = DMA_DIR_MemoryToPeripheral;
    init_type.DMA_BufferSize = LED_BUF_SZIE;
    init_type.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    init_type.DMA_MemoryInc = DMA_MemoryInc_Enable;
    init_type.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    init_type.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    init_type.DMA_Mode = DMA_Mode_Normal;
    init_type.DMA_Priority = DMA_Priority_High;
    init_type.DMA_FIFOMode = DMA_FIFOMode_Disable;
    init_type.DMA_FIFOThreshold = DMA_FIFOThreshold_Full;
    init_type.DMA_MemoryBurst = DMA_MemoryBurst_INC8;
    init_type.DMA_PeripheralBurst = DMA_PeripheralBurst_INC8;
    DMA_Init(DMA1_Stream4, &init_type);

    NVIC_InitTypeDef NVIC_InitStructure;
    NVIC_InitStructure.NVIC_IRQChannel = DMA1_Stream4_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 3;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 3;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
    DMA_ITConfig(DMA1_Stream4, DMA_IT_TC, ENABLE);

    return 0;
}

void DMA1_Stream4_IRQHandler(void)
{
    DMA_ClearITPendingBit(DMA1_Stream4, DMA_FLAG_TCIF4);

    sem_send_one(&g_led->sem);
}

static int nk60_v2_led_init(void)
{
    struct nk60_led *led;

    led = kmalloc(sizeof(struct nk60_led), GFP_KERNEL);
    if (led == NULL) {
        pr_err("alloc led buf error\r\n");
        return -ENOMEM;
    }
    g_led = led;
    led->buf = (uint8_t *)g_led_buf;

    nk60_v2_led_dma_init();
    nk60_v2_led_gpio_init();
    nk60_v2_led_spi_init();
    memset(led->buf, LED_DATA_0, LED_BUF_SZIE);

    mutex_init(&led->lock);
    sem_init(&led->sem, 0);
    led->enable = false;

    device_init(&led->dev);
    led->dev.name = "nk60_v2-led";
    led->dev.ops.write = nk60_v2_led_buf_write;
    led->dev.ops.control = nk60_v2_led_control;
    led->dev.priv = led;
    device_register(&led->dev);

    return 0;
}
task_init(nk60_v2_led_init);
