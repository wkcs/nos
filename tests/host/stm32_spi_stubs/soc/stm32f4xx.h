#ifndef NOS_TEST_STM32_SPI_VENDOR_H
#define NOS_TEST_STM32_SPI_VENDOR_H
#include <stdint.h>
typedef struct { int id; } SPI_TypeDef;
typedef struct { int id; } GPIO_TypeDef;
typedef struct { uint16_t GPIO_Pin; int GPIO_Mode, GPIO_OType, GPIO_Speed, GPIO_PuPd; } GPIO_InitTypeDef;
typedef struct { int SPI_Direction, SPI_Mode, SPI_DataSize, SPI_CPOL, SPI_CPHA, SPI_NSS;
                 int SPI_BaudRatePrescaler, SPI_FirstBit, SPI_CRCPolynomial; } SPI_InitTypeDef;
#define ENABLE 1
#define DISABLE 0
#define RESET 0
#define GPIO_Mode_AF 1
#define GPIO_Mode_OUT 2
#define GPIO_OType_PP 1
#define GPIO_Speed_100MHz 3
#define GPIO_PuPd_UP 1
#define SPI_Direction_2Lines_FullDuplex 1
#define SPI_Mode_Master 1
#define SPI_DataSize_8b 8
#define SPI_CPOL_Low 0
#define SPI_CPOL_High 1
#define SPI_CPHA_1Edge 0
#define SPI_CPHA_2Edge 1
#define SPI_NSS_Soft 1
#define SPI_BaudRatePrescaler_2 0
#define SPI_BaudRatePrescaler_4 8
#define SPI_BaudRatePrescaler_8 16
#define SPI_BaudRatePrescaler_16 24
#define SPI_BaudRatePrescaler_32 32
#define SPI_BaudRatePrescaler_64 40
#define SPI_BaudRatePrescaler_128 48
#define SPI_BaudRatePrescaler_256 56
#define SPI_FirstBit_MSB 1
#define SPI_I2S_FLAG_TXE 1
#define SPI_I2S_FLAG_RXNE 2
#define SPI_I2S_FLAG_BSY 3
void RCC_AHB1PeriphClockCmd(uint32_t, int);
void RCC_APB1PeriphClockCmd(uint32_t, int);
void RCC_APB2PeriphClockCmd(uint32_t, int);
void RCC_APB1PeriphResetCmd(uint32_t, int);
void RCC_APB2PeriphResetCmd(uint32_t, int);
void GPIO_Init(GPIO_TypeDef *, GPIO_InitTypeDef *);
void GPIO_PinAFConfig(GPIO_TypeDef *, uint16_t, uint8_t);
void GPIO_SetBits(GPIO_TypeDef *, uint16_t);
void GPIO_ResetBits(GPIO_TypeDef *, uint16_t);
void SPI_Init(SPI_TypeDef *, SPI_InitTypeDef *);
void SPI_Cmd(SPI_TypeDef *, int);
int SPI_I2S_GetFlagStatus(SPI_TypeDef *, uint16_t);
void SPI_I2S_SendData(SPI_TypeDef *, uint16_t);
uint16_t SPI_I2S_ReceiveData(SPI_TypeDef *);
#endif
