/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Author: Nick Hu
 * Simulated STM32 peripheral boundary for the real NK60 LED driver.
 */
#ifndef NK60_LED_TEST_PLATFORM_H
#define NK60_LED_TEST_PLATFORM_H
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#define __DMB() ((void)0)
#define CONFIG_SYS_TICK_MS 1
#define ENABLE 1
#define DISABLE 0
#define RESET 0
#define SET 1
#define GFP_KERNEL 0
#define unlikely(x) (x)
#define pr_err(...) ((void)0)
#define task_init(fn)
typedef uintptr_t addr_t;
struct mutex { int locked; };
struct device {
    void *priv;
    const char *name;
    struct {
        ssize_t (*write)(struct device *, addr_t, const void *, size_t);
        int (*control)(struct device *, int, void *);
    } ops;
};
struct nos_notification { uint64_t pending; };
#define NOS_WAIT_FOREVER UINT32_MAX
#define NOS_NO_WAIT 0U
struct display_dev_info { uint16_t width, height; };
enum { DS_COLOR_G, DS_COLOR_R, DS_COLOR_B, DS_COLOR_DATA_MAX };
enum { DS_CTRL_ENABLE, DS_CTRL_DISABLE, DS_CTRL_GET_ENABLE_STATUS,
       DS_CTRL_LOCK, DS_CTRL_UNLOCK, DS_CTRL_GET_DEV_INFO, DS_CTRL_REFRESH };
typedef struct { uint32_t CR1, DR; } SPI_TypeDef;
typedef struct { int enabled; unsigned count; } DMA_Stream_TypeDef;
typedef struct { int unused; } GPIO_TypeDef;
typedef struct {
    uint32_t GPIO_Pin, GPIO_Mode, GPIO_OType, GPIO_Speed, GPIO_PuPd;
} GPIO_InitTypeDef;
typedef struct {
    uint32_t SPI_Direction, SPI_Mode, SPI_DataSize, SPI_CPOL, SPI_CPHA,
             SPI_NSS, SPI_BaudRatePrescaler, SPI_FirstBit, SPI_CRCPolynomial;
} SPI_InitTypeDef;
typedef struct {
    uint32_t DMA_Channel, DMA_PeripheralBaseAddr, DMA_Memory0BaseAddr, DMA_DIR,
             DMA_BufferSize, DMA_PeripheralInc, DMA_MemoryInc,
             DMA_PeripheralDataSize, DMA_MemoryDataSize, DMA_Mode,
             DMA_Priority, DMA_FIFOMode, DMA_FIFOThreshold,
             DMA_MemoryBurst, DMA_PeripheralBurst;
} DMA_InitTypeDef;
typedef struct {
    uint32_t NVIC_IRQChannel, NVIC_IRQChannelPreemptionPriority,
             NVIC_IRQChannelSubPriority, NVIC_IRQChannelCmd;
} NVIC_InitTypeDef;
typedef struct { uint32_t PCLK1_Frequency; } RCC_ClocksTypeDef;
static SPI_TypeDef spi;
static DMA_Stream_TypeDef stream;
static GPIO_TypeDef gpio;
static unsigned power_pin;
#define SPI2 (&spi)
#define DMA1_Stream4 (&stream)
#define GPIOB (&gpio)
#define GPIOC (&gpio)
#define PBout(n) power_pin
#define IS_SPI_BAUDRATE_PRESCALER(x) 1
#define assert_param(x) assert(x)
#define RCC_AHB1Periph_GPIOC 2U
#define RCC_AHB1Periph_GPIOB 3U
#define RCC_APB1Periph_SPI2 4U
#define RCC_AHB1Periph_DMA1 5U
#define GPIO_Pin_0 6U
#define GPIO_Pin_3 7U
#define GPIO_Mode_AF 8U
#define GPIO_Mode_OUT 9U
#define GPIO_OType_PP 10U
#define GPIO_Speed_100MHz 11U
#define GPIO_PuPd_DOWN 12U
#define GPIO_PuPd_UP 13U
#define GPIO_PinSource3 14U
#define GPIO_AF_SPI2 15U
#define SPI_Direction_1Line_Tx 16U
#define SPI_Mode_Master 17U
#define SPI_DataSize_8b 18U
#define SPI_CPOL_High 19U
#define SPI_CPOL_Low 20U
#define SPI_CPHA_2Edge 21U
#define SPI_CPHA_1Edge 22U
#define SPI_NSS_Soft 23U
#define SPI_FirstBit_MSB 24U
#define SPI_I2S_DMAReq_Tx 25U
#define DMA_Channel_0 26U
#define DMA_DIR_MemoryToPeripheral 27U
#define DMA_PeripheralInc_Disable 28U
#define DMA_MemoryInc_Enable 29U
#define DMA_PeripheralDataSize_Byte 30U
#define DMA_MemoryDataSize_Byte 31U
#define DMA_Mode_Normal 32U
#define DMA_Priority_High 33U
#define DMA_Priority_VeryHigh 34U
#define DMA_FIFOMode_Disable 35U
#define DMA_FIFOMode_Enable 36U
#define DMA_FIFOThreshold_Full 37U
#define DMA_MemoryBurst_INC8 38U
#define DMA_PeripheralBurst_INC8 39U
#define DMA_MemoryBurst_Single 40U
#define DMA_PeripheralBurst_Single 41U
#define DMA1_Stream4_IRQn 42U
#define SPI_I2S_FLAG_TXE 43U
#define SPI_I2S_FLAG_BSY 44U
#define SPI_BaudRatePrescaler_4 4U
#define SPI_BaudRatePrescaler_8 8U
#define DMA_FLAG_TCIF4 1U
#define DMA_FLAG_TEIF4 2U
#define DMA_FLAG_DMEIF4 4U
#define DMA_FLAG_FEIF4 8U
#define DMA_FLAG_HTIF4 16U
#define DMA_IT_TCIF4 DMA_FLAG_TCIF4
#define DMA_IT_TEIF4 DMA_FLAG_TEIF4
#define DMA_IT_DMEIF4 DMA_FLAG_DMEIF4
#define DMA_IT_FEIF4 DMA_FLAG_FEIF4
#define DMA_IT_TC 1U
#define DMA_IT_TE 2U
#define DMA_IT_DME 4U
#define DMA_IT_FE 8U
static SPI_InitTypeDef spi_config;
static DMA_InitTypeDef dma_config;
static unsigned dma_flags, transfers, busy_polls, busy_left, reset_delay;
static uint32_t pclk = 42000000U;
static int transfer_error, wait_error, stuck_spi;
static uint64_t fake_time;
static void mutex_init(struct mutex *m) { m->locked = 0; }
static void mutex_lock(struct mutex *m) { assert(!m->locked); m->locked = 1; }
static void mutex_unlock(struct mutex *m) { assert(m->locked); m->locked = 0; }
static void *kmalloc(size_t n, int flags) { (void)flags; return calloc(1, n); }
static void kfree(void *p) { free(p); }
static void device_init(struct device *d) { (void)d; }
static void device_register(struct device *d) { (void)d; }
static void usleep(uint32_t us) { reset_delay += us; }
static uint64_t cpu_run_time_us(void) { return fake_time++; }
static uint32_t msec_to_tick(uint32_t ms) { return ms; }
static void nos_notification_init(struct nos_notification *n) { n->pending = 0; }
static void nos_notify_signal_obj(struct nos_notification *n, uint64_t bits) { n->pending |= bits; }
static int nos_notify_wait_obj(struct nos_notification *, uint64_t *, uint32_t);
static void RCC_GetClocksFreq(RCC_ClocksTypeDef *c) { c->PCLK1_Frequency = pclk; }
#define RCC_AHB1PeriphClockCmd(a, b) ((void)0)
#define RCC_APB1PeriphClockCmd(a, b) ((void)0)
#define RCC_APB1PeriphResetCmd(a, b) ((void)0)
#define GPIO_Init(a, b) ((void)0)
#define GPIO_PinAFConfig(a, b, c) ((void)0)
#define GPIO_ResetBits(a, b) ((void)0)
#define SPI_Cmd(a, b) ((void)0)
#define SPI_I2S_DMACmd(a, b, c) ((void)0)
#define SPI_I2S_SendData(a, b) ((void)0)
#define DMA_DeInit(a) ((void)0)
#define NVIC_Init(a) ((void)0)
#define NVIC_ClearPendingIRQ(a) ((void)0)
#define DMA_ITConfig(a, b, c) ((void)0)
static void SPI_Init(SPI_TypeDef *s, SPI_InitTypeDef *c) { (void)s; spi_config = *c; }
static int SPI_I2S_GetFlagStatus(SPI_TypeDef *s, unsigned flag)
{
    (void)s;
    if (flag == SPI_I2S_FLAG_TXE) return SET;
    busy_polls++;
    if (stuck_spi) return SET;
    if (busy_left) { busy_left--; return SET; }
    return RESET;
}
static void DMA_Init(DMA_Stream_TypeDef *s, DMA_InitTypeDef *c) { (void)s; dma_config = *c; }
static void DMA_Cmd(DMA_Stream_TypeDef *s, int on)
{
    s->enabled = on;
    if (on) { transfers++; busy_left = 3; }
}
static int DMA_GetCmdStatus(DMA_Stream_TypeDef *s) { return s->enabled; }
static void DMA_SetCurrDataCounter(DMA_Stream_TypeDef *s, unsigned n) { s->count = n; }
static int DMA_GetITStatus(DMA_Stream_TypeDef *s, unsigned f) { (void)s; return !!(dma_flags & f); }
static int DMA_GetFlagStatus(DMA_Stream_TypeDef *s, unsigned f) { (void)s; return !!(dma_flags & f); }
static void DMA_ClearFlag(DMA_Stream_TypeDef *s, unsigned f) { (void)s; dma_flags &= ~f; }
static void DMA_ClearITPendingBit(DMA_Stream_TypeDef *s, unsigned f) { DMA_ClearFlag(s, f); }
#endif
