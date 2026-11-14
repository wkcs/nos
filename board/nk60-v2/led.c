/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#define pr_fmt(fmt) "[LED]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/errno.h>
#include <kernel/sleep.h>
#include <kernel/cpu.h>
#include <kernel/device.h>
#include <kernel/init.h>
#include <kernel/gpio.h>
#include <kernel/mm.h>
#include <kernel/mutex.h>
#include <string.h>
#include <board/board.h>
#include <nos/notification.h>
#include <nos/timeout.h>

#include <drivers/led.h>
#include <nos/display.h>

#define LED_VCC_EN PBout(0)

/*
 * SPI2 = APB1 / 4 = 10.5 MHz. Each WS2812B bit occupies 12 SPI bits:
 *   0: 111000000000 -> T0H = 286 ns, T0L = 857 ns
 *   1: 111111110000 -> T1H = 762 ns, T1L = 381 ns
 * These fit the original WS2812B timing windows. The old 8-bit C0/F0
 * symbols at 5.25 MHz stretched T0L/T1L to 1143/762 ns, outside spec.
 */
#define LED_DATA_0 0xE00U
#define LED_DATA_1 0xFF0U
#define LED_COLOR_BYTES 12U
#define LED_SPI_CLOCK 10500000U
#define LED_RESET_US 300U
/* 400 zero bytes at 10.5 MHz hold DIN low for 304.8 us (> 280 us). */
#define LED_RESET_BYTES 400U
#define LED_NUM 61U
#define LED_DATA_SIZE (LED_NUM * COLOR_MAX * LED_COLOR_BYTES)
#define LED_BUF_SIZE (LED_RESET_BYTES + LED_DATA_SIZE + LED_RESET_BYTES)
#define LED_DMA_FLAGS (DMA_FLAG_TCIF4 | DMA_FLAG_TEIF4 | DMA_FLAG_DMEIF4 | \
                       DMA_FLAG_FEIF4 | DMA_FLAG_HTIF4)
#define LED_NOTIFY_DONE 1U
#define LED_NOTIFY_ERROR 2U
#define LED_REFRESH_TIMEOUT_TICKS ((20U + CONFIG_SYS_TICK_MS - 1U) / CONFIG_SYS_TICK_MS)

#define LED_WIDTH 14
#define LED_HEIGHT 5

static uint8_t g_led_buf[LED_BUF_SIZE];

struct nk60_led {
    struct mutex lock;
    struct device dev;
    struct nos_notification notify;

    bool enable;
};
struct nk60_led *g_led;

static const unsigned int g_nk60_led_buf_index[LED_WIDTH * LED_HEIGHT] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13,
    14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27,
    28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 40,
    41, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 52,
    53, 54, 55, 56, 56, 56, 56, 56, 56, 56, 57, 58, 59, 60
};

static void nk60_v2_led_dma_stop(void)
{
    SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Tx, DISABLE);
    DMA_Cmd(DMA1_Stream4, DISABLE);
    /* NDTR may only be reloaded after the stream acknowledges disable. */
    while (DMA_GetCmdStatus(DMA1_Stream4) != DISABLE) {
    }
}

static void nk60_v2_led_dma_start(void)
{
    nk60_v2_led_dma_stop();
    DMA_ClearFlag(DMA1_Stream4, LED_DMA_FLAGS);
    DMA_SetCurrDataCounter(DMA1_Stream4, LED_BUF_SIZE);
    __DMB();
    DMA_Cmd(DMA1_Stream4, ENABLE);
    SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Tx, ENABLE);
}

static int nk60_v2_led_spi_drain(void)
{
    uint64_t start = cpu_run_time_us();

    /* DMA TC only means the last byte reached DR, not the output pin. */
    while (SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_TXE) == RESET ||
           SPI_I2S_GetFlagStatus(SPI2, SPI_I2S_FLAG_BSY) != RESET) {
        if (cpu_run_time_us() - start >= 1000U) {
            return -ETIMEDOUT;
        }
    }
    return 0;
}

static void nk60_v2_led_write_single_color_buf(uint8_t data, uint8_t color_type,
                                              unsigned int index)
{
    uint8_t *out = g_led_buf + LED_RESET_BYTES +
                   (index * COLOR_MAX + color_type) * LED_COLOR_BYTES;
    unsigned int i;

    /* Pack two 12-bit symbols into three bytes, MSB first, with no gaps. */
    for (i = 0; i < 4U; i++) {
        uint16_t first = (data & 0x80U) ? LED_DATA_1 : LED_DATA_0;
        uint16_t second = (data & 0x40U) ? LED_DATA_1 : LED_DATA_0;

        *out++ = first >> 4;
        *out++ = (uint8_t)((first << 4) | (second >> 8));
        *out++ = (uint8_t)second;
        data <<= 2;
    }
}

static void nk60_v2_led_write_buf(uint8_t r, uint8_t g, uint8_t b, int index)
{
    if (index < 0 || (unsigned int)index >= LED_NUM) {
        return;
    }
    nk60_v2_led_write_single_color_buf(g, COLOR_G, index);
    nk60_v2_led_write_single_color_buf(r, COLOR_R, index);
    nk60_v2_led_write_single_color_buf(b, COLOR_B, index);
}

static ssize_t nk60_v2_led_buf_write(struct device *dev, addr_t pos, const void *buffer, size_t size)
{
    struct nk60_led *led = dev->priv;
    const uint8_t *buf = buffer;
    int i, index;

    if (buffer == NULL || size != LED_WIDTH * LED_HEIGHT * DS_COLOR_DATA_MAX) {
        pr_err("data size error\n");
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

/* Caller holds the device lock, or is initializing the unpublished device. */
static void nk60_v2_led_buf_init(void)
{
    unsigned int i;

    /* Reset intervals are raw low levels, not encoded zero-valued pixels. */
    memset(g_led_buf, 0, sizeof(g_led_buf));
    for (i = 0; i < LED_NUM; i++) {
        nk60_v2_led_write_buf(0, 0, 0, i);
    }
}

static int nk60_v2_led_spi_init(void);

static int nk60_v2_led_control(struct device *dev, int cmd, void *args)
{
    struct nk60_led *led = dev->priv;
    struct display_dev_info info = {.width = LED_WIDTH, .height = LED_HEIGHT};
    int ret = 0;

    mutex_lock(&led->lock);
    switch (cmd) {
    case LED_CTRL_ENABLE:
        if (!led->enable) {
            LED_VCC_EN = 1;
            /* Allow power-on reset before accepting the first frame. */
            usleep(LED_RESET_US);
            led->enable = true;
        }
        break;
    case LED_CTRL_DISABLE:
        led->enable = false;
        LED_VCC_EN = 0;
        nk60_v2_led_buf_init();
        break;
    case LED_CTRL_GET_ENABLE_STATUS:
        ret = led->enable;
        break;
    case LED_CTRL_REFRESH: {
        uint64_t bits;

        if (!led->enable) {
            break;
        }
        /* Discard any completion left by an aborted transfer. */
        nk60_v2_led_dma_stop();
        DMA_ClearFlag(DMA1_Stream4, LED_DMA_FLAGS);
        while (nos_notify_wait_obj(&led->notify, &bits, NOS_NO_WAIT) == 0) {
        }
        nk60_v2_led_dma_start();
        ret = nos_notify_wait_obj(&led->notify, &bits, LED_REFRESH_TIMEOUT_TICKS);
        nk60_v2_led_dma_stop();
        if (ret == 0 && (bits & LED_NOTIFY_ERROR)) {
            ret = -EIO;
        }
        if (ret == 0) {
            ret = nk60_v2_led_spi_drain();
        }
        if (ret != 0) {
            /* Abort a partial frame and restore low idle before another try. */
            nk60_v2_led_spi_init();
            usleep(LED_RESET_US);
        }
        break;
    }
    case DS_CTRL_GET_DEV_INFO:
        if (args == NULL) {
            ret = -EINVAL;
            break;
        }
        memcpy(args, &info, sizeof(info));
        break;
    default:
        pr_err("unknown cmd: %d\r\n", cmd);
        ret = -EINVAL;
        break;
    }
    mutex_unlock(&led->lock);
    return ret;
}

static int nk60_v2_led_spi_init(void)
{
    GPIO_InitTypeDef GPIO_InitStructure;
    SPI_InitTypeDef SPI_InitStructure;
    RCC_ClocksTypeDef clocks;

    RCC_GetClocksFreq(&clocks);
    if (clocks.PCLK1_Frequency != LED_SPI_CLOCK * 4U) {
        pr_err("unsupported LED SPI clock: %lu\r\n",
               (unsigned long)clocks.PCLK1_Frequency);
        return -EINVAL;
    }

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
    SPI_InitStructure.SPI_CPOL = SPI_CPOL_Low;
    SPI_InitStructure.SPI_CPHA = SPI_CPHA_1Edge;
    SPI_InitStructure.SPI_NSS = SPI_NSS_Soft;
    SPI_InitStructure.SPI_BaudRatePrescaler = SPI_BaudRatePrescaler_4;
    SPI_InitStructure.SPI_FirstBit = SPI_FirstBit_MSB;
    SPI_InitStructure.SPI_CRCPolynomial = 7;
    SPI_Init(SPI2, &SPI_InitStructure);

    SPI_Cmd(SPI2, ENABLE);
    /* Prime MOSI low for startup, bootloader handoff and error recovery. */
    SPI_I2S_SendData(SPI2, 0);
    return nk60_v2_led_spi_drain();
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
    NVIC_InitTypeDef NVIC_InitStructure;

    RCC_AHB1PeriphClockCmd(RCC_AHB1Periph_DMA1, ENABLE);
    DMA_DeInit(DMA1_Stream4);

    init_type.DMA_Channel = DMA_Channel_0;
    init_type.DMA_PeripheralBaseAddr = (uint32_t)&SPI2->DR;
    init_type.DMA_Memory0BaseAddr = (uint32_t)g_led_buf;
    init_type.DMA_DIR = DMA_DIR_MemoryToPeripheral;
    init_type.DMA_BufferSize = LED_BUF_SIZE;
    init_type.DMA_PeripheralInc = DMA_PeripheralInc_Disable;
    init_type.DMA_MemoryInc = DMA_MemoryInc_Enable;
    init_type.DMA_PeripheralDataSize = DMA_PeripheralDataSize_Byte;
    init_type.DMA_MemoryDataSize = DMA_MemoryDataSize_Byte;
    init_type.DMA_Mode = DMA_Mode_Normal;
    init_type.DMA_Priority = DMA_Priority_VeryHigh;
    init_type.DMA_FIFOMode = DMA_FIFOMode_Enable;
    init_type.DMA_FIFOThreshold = DMA_FIFOThreshold_Full;
    init_type.DMA_MemoryBurst = DMA_MemoryBurst_Single;
    init_type.DMA_PeripheralBurst = DMA_PeripheralBurst_Single;
    DMA_Init(DMA1_Stream4, &init_type);

    DMA_ClearFlag(DMA1_Stream4, LED_DMA_FLAGS);
    NVIC_ClearPendingIRQ(DMA1_Stream4_IRQn);
    NVIC_InitStructure.NVIC_IRQChannel = DMA1_Stream4_IRQn;
    NVIC_InitStructure.NVIC_IRQChannelPreemptionPriority = 3;
    NVIC_InitStructure.NVIC_IRQChannelSubPriority = 3;
    NVIC_InitStructure.NVIC_IRQChannelCmd = ENABLE;
    NVIC_Init(&NVIC_InitStructure);
    DMA_ITConfig(DMA1_Stream4, DMA_IT_TC | DMA_IT_TE | DMA_IT_DME | DMA_IT_FE, ENABLE);

    return 0;
}

void DMA1_Stream4_IRQHandler(void)
{
    uint64_t bits = 0;

    if (DMA_GetITStatus(DMA1_Stream4, DMA_IT_TEIF4) != RESET ||
        DMA_GetITStatus(DMA1_Stream4, DMA_IT_DMEIF4) != RESET ||
        DMA_GetITStatus(DMA1_Stream4, DMA_IT_FEIF4) != RESET) {
        bits = LED_NOTIFY_ERROR;
    } else if (DMA_GetITStatus(DMA1_Stream4, DMA_IT_TCIF4) != RESET) {
        bits = LED_NOTIFY_DONE;
    }
    DMA_ClearFlag(DMA1_Stream4, LED_DMA_FLAGS);
    if (bits != 0) {
        SPI_I2S_DMACmd(SPI2, SPI_I2S_DMAReq_Tx, DISABLE);
        nos_notify_signal_obj(&g_led->notify, bits);
    }
}

static int nk60_v2_led_init(void)
{
    struct nk60_led *led;
    int ret;

    led = kmalloc(sizeof(struct nk60_led), GFP_KERNEL);
    if (led == NULL) {
        pr_err("alloc led buf error\n");
        return -ENOMEM;
    }
    mutex_init(&led->lock);
    nos_notification_init(&led->notify);
    led->enable = false;
    g_led = led;

    nk60_v2_led_gpio_init();
    ret = nk60_v2_led_spi_init();
    if (ret != 0) {
        g_led = NULL;
        kfree(led);
        return ret;
    }
    nk60_v2_led_buf_init();
    nk60_v2_led_dma_init();

    device_init(&led->dev);
    led->dev.name = "nk60_v2-led";
    led->dev.ops.write = nk60_v2_led_buf_write;
    led->dev.ops.control = nk60_v2_led_control;
    led->dev.priv = led;
    device_register(&led->dev);

    return 0;
}
task_init(nk60_v2_led_init);
