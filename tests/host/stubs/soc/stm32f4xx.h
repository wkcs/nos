/**
 * Copyright (C) 2026 Nick Hu
 *
 * Minimal USART peripheral model for host-side driver tests.
 */
#ifndef NOS_TEST_STM32F4_H
#define NOS_TEST_STM32F4_H
#include <stdint.h>
typedef struct { uint32_t unused; } USART_TypeDef;
typedef struct { uint32_t unused; } GPIO_TypeDef;
typedef struct { uint32_t unused; } DMA_Stream_TypeDef;
typedef struct {
    uint32_t GPIO_Pin, GPIO_Speed, GPIO_Mode, GPIO_OType, GPIO_PuPd;
} GPIO_InitTypeDef;
typedef struct {
    uint32_t USART_BaudRate, USART_WordLength, USART_StopBits, USART_Parity;
    uint32_t USART_HardwareFlowControl, USART_Mode;
} USART_InitTypeDef;
typedef struct {
    uint32_t NVIC_IRQChannel, NVIC_IRQChannelPreemptionPriority;
    uint32_t NVIC_IRQChannelSubPriority, NVIC_IRQChannelCmd;
} NVIC_InitTypeDef;
extern USART_TypeDef simulated_usart;
extern GPIO_TypeDef simulated_gpio;
extern DMA_Stream_TypeDef simulated_dma;
extern int simulated_interrupt_context;
#define USART1 (&simulated_usart)
#define GPIOA (&simulated_gpio)
#define DMA2_Stream7 (&simulated_dma)
#define RCC_APB2Periph_USART1 1U
#define RCC_AHB1Periph_GPIOA 1U
#define GPIO_Pin_9 0x200U
#define GPIO_Pin_10 0x400U
#define GPIO_PinSource9 9U
#define GPIO_PinSource10 10U
#define GPIO_AF_USART1 7U
#define GPIO_Speed_50MHz 1U
#define GPIO_Mode_AF 1U
#define GPIO_OType_PP 1U
#define GPIO_PuPd_UP 1U
#define USART_WordLength_8b 0U
#define USART_StopBits_1 0U
#define USART_Parity_No 0U
#define USART_HardwareFlowControl_None 0U
#define USART_Mode_Rx 1U
#define USART_Mode_Tx 2U
#define USART_FLAG_TC 1U
#define USART_FLAG_TXE 2U
#define USART_FLAG_RXNE 4U
#define USART_IT_TXE 1U
#define USART_IT_RXNE 2U
#define USART_DMAReq_Tx 1U
#define DMA_IT_TCIF7 1U
#define DMA_IT_TEIF7 2U
#define DMA_FLAG_TCIF7 1U
#define DMA_FLAG_TEIF7 2U
#define DMA_IT_TE 2U
#define USART1_IRQn 37U
#define RESET 0
#define SET 1
#define DISABLE 0
#define ENABLE 1
int USART_GetFlagStatus(USART_TypeDef *regs, uint32_t flag);
int USART_GetITStatus(USART_TypeDef *regs, uint32_t irq);
void USART_SendData(USART_TypeDef *regs, uint16_t value);
uint16_t USART_ReceiveData(USART_TypeDef *regs);
void USART_ITConfig(USART_TypeDef *regs, uint32_t irq, int enabled);
void USART_Init(USART_TypeDef *regs, USART_InitTypeDef *config);
void USART_Cmd(USART_TypeDef *regs, int enabled);
void RCC_AHB1PeriphClockCmd(uint32_t clock, int enabled);
void RCC_APB2PeriphClockCmd(uint32_t clock, int enabled);
void GPIO_PinAFConfig(GPIO_TypeDef *gpio, uint8_t pin, uint8_t af);
void GPIO_Init(GPIO_TypeDef *gpio, GPIO_InitTypeDef *config);
void NVIC_Init(NVIC_InitTypeDef *config);
void USART_DMACmd(USART_TypeDef *regs, uint32_t request, int enabled);
void DMA_Cmd(DMA_Stream_TypeDef *stream, int enabled);
int DMA_GetCmdStatus(DMA_Stream_TypeDef *stream);
void DMA_SetCurrDataCounter(DMA_Stream_TypeDef *stream, uint16_t count);
uint16_t DMA_GetCurrDataCounter(DMA_Stream_TypeDef *stream);
int DMA_GetITStatus(DMA_Stream_TypeDef *stream, uint32_t irq);
void DMA_ClearITPendingBit(DMA_Stream_TypeDef *stream, uint32_t irq);
void DMA_ITConfig(DMA_Stream_TypeDef *stream, uint32_t irq, int enabled);
static inline uint32_t __get_IPSR(void) { return (uint32_t)simulated_interrupt_context; }
#endif
