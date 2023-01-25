/**
 * Copyright (C) 2023-2023 胡启航<Nick Hu>
 *
 * Author: 胡启航<Nick Hu>
 *
 * Email: huqihan@live.com
 */

#include <kernel/kernel.h>
#include <kernel/mm.h>
#include "board.h"

#define EXR_SRAM_ADDR ((addr_t)(0x68000000))
#define EXR_SRAM_SIZE (1024 * 1024) /* 1m */

extern addr_t __mm_pool_start, __mm_pool_end;
extern addr_t __mm_sys_reserve_start, __mm_sys_reserve_end;

mm_reserve_node_register((addr_t)&__mm_sys_reserve_start, (addr_t)&__mm_sys_reserve_end, reserve);
mm_node_register((addr_t)&__mm_pool_start, (addr_t)&__mm_pool_end, inside);
mm_node_register(EXR_SRAM_ADDR, EXR_SRAM_ADDR + EXR_SRAM_SIZE, external);

void board_mm_init(void)
{
    FSMC_NORSRAMInitTypeDef  FSMC_NORSRAMInitStructure;
    FSMC_NORSRAMTimingInitTypeDef  readWriteTiming;
    GPIO_InitTypeDef  GPIO_InitStructure;

    RCC_APB2PeriphClockCmd(RCC_APB2Periph_GPIOD|RCC_APB2Periph_GPIOE|RCC_APB2Periph_GPIOF|RCC_APB2Periph_GPIOG,ENABLE);
    RCC_AHBPeriphClockCmd(RCC_AHBPeriph_FSMC,ENABLE);

    GPIO_InitStructure.GPIO_Pin = 0xFF33; 			 	//PORTD复用推挽输出
    GPIO_InitStructure.GPIO_Mode = GPIO_Mode_AF_PP; 		 //复用推挽输出
    GPIO_InitStructure.GPIO_Speed = GPIO_Speed_50MHz;
    GPIO_Init(GPIOD, &GPIO_InitStructure);


    GPIO_InitStructure.GPIO_Pin = 0xFF83; 			 	//PORTE复用推挽输出
    GPIO_Init(GPIOE, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = 0xF03F; 			 	//PORTD复用推挽输出
    GPIO_Init(GPIOF, &GPIO_InitStructure);

    GPIO_InitStructure.GPIO_Pin = 0x043F; 			 	//PORTD复用推挽输出
    GPIO_Init(GPIOG, &GPIO_InitStructure);


    readWriteTiming.FSMC_AddressSetupTime = 0x00;	 //地址建立时间（ADDSET）为1个HCLK 1/36M=27ns
    readWriteTiming.FSMC_AddressHoldTime = 0x00;	 //地址保持时间（ADDHLD）模式A未用到
    readWriteTiming.FSMC_DataSetupTime = 0x03;		 //数据保持时间（DATAST）为3个HCLK 4/72M=55ns(对EM的SRAM芯片)
    readWriteTiming.FSMC_BusTurnAroundDuration = 0x00;
    readWriteTiming.FSMC_CLKDivision = 0x00;
    readWriteTiming.FSMC_DataLatency = 0x00;
    readWriteTiming.FSMC_AccessMode = FSMC_AccessMode_A;	 //模式A



    FSMC_NORSRAMInitStructure.FSMC_Bank = FSMC_Bank1_NORSRAM3;//  这里我们使用NE3 ，也就对应BTCR[4],[5]。
    FSMC_NORSRAMInitStructure.FSMC_DataAddressMux = FSMC_DataAddressMux_Disable;
    FSMC_NORSRAMInitStructure.FSMC_MemoryType =FSMC_MemoryType_SRAM;// FSMC_MemoryType_SRAM;  //SRAM
    FSMC_NORSRAMInitStructure.FSMC_MemoryDataWidth = FSMC_MemoryDataWidth_16b;//存储器数据宽度为16bit
    FSMC_NORSRAMInitStructure.FSMC_BurstAccessMode =FSMC_BurstAccessMode_Disable;// FSMC_BurstAccessMode_Disable;
    FSMC_NORSRAMInitStructure.FSMC_WaitSignalPolarity = FSMC_WaitSignalPolarity_Low;
	FSMC_NORSRAMInitStructure.FSMC_AsynchronousWait=FSMC_AsynchronousWait_Disable;
    FSMC_NORSRAMInitStructure.FSMC_WrapMode = FSMC_WrapMode_Disable;
    FSMC_NORSRAMInitStructure.FSMC_WaitSignalActive = FSMC_WaitSignalActive_BeforeWaitState;
    FSMC_NORSRAMInitStructure.FSMC_WriteOperation = FSMC_WriteOperation_Enable;	//存储器写使能
    FSMC_NORSRAMInitStructure.FSMC_WaitSignal = FSMC_WaitSignal_Disable;
    FSMC_NORSRAMInitStructure.FSMC_ExtendedMode = FSMC_ExtendedMode_Disable; // 读写使用相同的时序
    FSMC_NORSRAMInitStructure.FSMC_WriteBurst = FSMC_WriteBurst_Disable;
    FSMC_NORSRAMInitStructure.FSMC_ReadWriteTimingStruct = &readWriteTiming;
    FSMC_NORSRAMInitStructure.FSMC_WriteTimingStruct = &readWriteTiming; //读写同样时序

    FSMC_NORSRAMInit(&FSMC_NORSRAMInitStructure);  //初始化FSMC配置

    FSMC_NORSRAMCmd(FSMC_Bank1_NORSRAM3, ENABLE);  // 使能BANK3
}
