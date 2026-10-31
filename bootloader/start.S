/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Bootloader: Cortex-M4 启动与中断向量表
 */

    .syntax unified
    .cpu cortex-m4
    .fpu fpv4-sp-d16
    .thumb

    .global g_pfnVectors
    .global reset_handler
    .global default_handler

    .section .isr_vector, "a", %progbits
    .type g_pfnVectors, %object
g_pfnVectors:
    .word _estack
    .word reset_handler
    .word NMI_Handler
    .word HardFault_Handler
    .word MemManage_Handler
    .word BusFault_Handler
    .word UsageFault_Handler
    .word 0
    .word 0
    .word 0
    .word 0
    .word SVC_Handler
    .word DebugMon_Handler
    .word 0
    .word PendSV_Handler
    .word SysTick_Handler

    /* STM32F401 外设中断向量 */
    .word WWDG_IRQHandler
    .word PVD_IRQHandler
    .word TAMP_STAMP_IRQHandler
    .word RTC_WKUP_IRQHandler
    .word FLASH_IRQHandler
    .word RCC_IRQHandler
    .word EXTI0_IRQHandler
    .word EXTI1_IRQHandler
    .word EXTI2_IRQHandler
    .word EXTI3_IRQHandler
    .word EXTI4_IRQHandler
    .word DMA1_Stream0_IRQHandler
    .word DMA1_Stream1_IRQHandler
    .word DMA1_Stream2_IRQHandler
    .word DMA1_Stream3_IRQHandler
    .word DMA1_Stream4_IRQHandler
    .word DMA1_Stream5_IRQHandler
    .word DMA1_Stream6_IRQHandler
    .word ADC_IRQHandler
    .word 0
    .word 0
    .word 0
    .word 0
    .word EXTI9_5_IRQHandler
    .word TIM1_BRK_TIM9_IRQHandler
    .word TIM1_UP_TIM10_IRQHandler
    .word TIM1_TRG_COM_TIM11_IRQHandler
    .word TIM1_CC_IRQHandler
    .word TIM2_IRQHandler
    .word TIM3_IRQHandler
    .word TIM4_IRQHandler
    .word I2C1_EV_IRQHandler
    .word I2C1_ER_IRQHandler
    .word I2C2_EV_IRQHandler
    .word I2C2_ER_IRQHandler
    .word SPI1_IRQHandler
    .word SPI2_IRQHandler
    .word USART1_IRQHandler
    .word USART2_IRQHandler
    .word 0
    .word EXTI15_10_IRQHandler
    .word RTC_Alarm_IRQHandler
    .word OTG_FS_WKUP_IRQHandler
    .word 0
    .word 0
    .word 0
    .word 0
    .word DMA1_Stream7_IRQHandler
    .word 0
    .word SDIO_IRQHandler
    .word TIM5_IRQHandler
    .word SPI3_IRQHandler
    .word 0
    .word 0
    .word 0
    .word 0
    .word DMA2_Stream0_IRQHandler
    .word DMA2_Stream1_IRQHandler
    .word DMA2_Stream2_IRQHandler
    .word DMA2_Stream3_IRQHandler
    .word DMA2_Stream4_IRQHandler
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0
    .word OTG_FS_IRQHandler
    .word DMA2_Stream5_IRQHandler
    .word DMA2_Stream6_IRQHandler
    .word DMA2_Stream7_IRQHandler
    .word USART6_IRQHandler
    .word I2C3_EV_IRQHandler
    .word I2C3_ER_IRQHandler
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0
    .word 0
    .word FPU_IRQHandler
    .word 0
    .word 0
    .word SPI4_IRQHandler

    .size g_pfnVectors, . - g_pfnVectors

    .section .text.reset_handler, "ax", %progbits
    .type reset_handler, %function
reset_handler:
    cpsid i
    ldr sp, =_estack

    /* 搬运 .data 和 .ramfunc */
    ldr r0, =_sdata
    ldr r1, =_edata
    ldr r2, =_sidata
1:
    cmp r0, r1
    bhs 2f
    ldr r3, [r2], #4
    str r3, [r0], #4
    b 1b

2:
    /* 清零 .bss */
    ldr r0, =_sbss
    ldr r1, =_ebss
    movs r2, #0
3:
    cmp r0, r1
    bhs 4f
    str r2, [r0], #4
    b 3b

4:
    /* 跳转 main 主函数 */
    cpsie i
    bl main
5:
    b 5b
    .size reset_handler, . - reset_handler

    .section .text.default_handler, "ax", %progbits
    .type default_handler, %function
default_handler:
    b default_handler
    .size default_handler, . - default_handler

    /* 弱别名声明，默认指向 default_handler */
    .weak NMI_Handler
    .thumb_set NMI_Handler, default_handler
    .weak HardFault_Handler
    .thumb_set HardFault_Handler, default_handler
    .weak MemManage_Handler
    .thumb_set MemManage_Handler, default_handler
    .weak BusFault_Handler
    .thumb_set BusFault_Handler, default_handler
    .weak UsageFault_Handler
    .thumb_set UsageFault_Handler, default_handler
    .weak SVC_Handler
    .thumb_set SVC_Handler, default_handler
    .weak DebugMon_Handler
    .thumb_set DebugMon_Handler, default_handler
    .weak PendSV_Handler
    .thumb_set PendSV_Handler, default_handler
    .weak SysTick_Handler
    .thumb_set SysTick_Handler, default_handler

    .weak WWDG_IRQHandler
    .thumb_set WWDG_IRQHandler, default_handler
    .weak PVD_IRQHandler
    .thumb_set PVD_IRQHandler, default_handler
    .weak TAMP_STAMP_IRQHandler
    .thumb_set TAMP_STAMP_IRQHandler, default_handler
    .weak RTC_WKUP_IRQHandler
    .thumb_set RTC_WKUP_IRQHandler, default_handler
    .weak FLASH_IRQHandler
    .thumb_set FLASH_IRQHandler, default_handler
    .weak RCC_IRQHandler
    .thumb_set RCC_IRQHandler, default_handler
    .weak EXTI0_IRQHandler
    .thumb_set EXTI0_IRQHandler, default_handler
    .weak EXTI1_IRQHandler
    .thumb_set EXTI1_IRQHandler, default_handler
    .weak EXTI2_IRQHandler
    .thumb_set EXTI2_IRQHandler, default_handler
    .weak EXTI3_IRQHandler
    .thumb_set EXTI3_IRQHandler, default_handler
    .weak EXTI4_IRQHandler
    .thumb_set EXTI4_IRQHandler, default_handler
    .weak DMA1_Stream0_IRQHandler
    .thumb_set DMA1_Stream0_IRQHandler, default_handler
    .weak DMA1_Stream1_IRQHandler
    .thumb_set DMA1_Stream1_IRQHandler, default_handler
    .weak DMA1_Stream2_IRQHandler
    .thumb_set DMA1_Stream2_IRQHandler, default_handler
    .weak DMA1_Stream3_IRQHandler
    .thumb_set DMA1_Stream3_IRQHandler, default_handler
    .weak DMA1_Stream4_IRQHandler
    .thumb_set DMA1_Stream4_IRQHandler, default_handler
    .weak DMA1_Stream5_IRQHandler
    .thumb_set DMA1_Stream5_IRQHandler, default_handler
    .weak DMA1_Stream6_IRQHandler
    .thumb_set DMA1_Stream6_IRQHandler, default_handler
    .weak ADC_IRQHandler
    .thumb_set ADC_IRQHandler, default_handler
    .weak EXTI9_5_IRQHandler
    .thumb_set EXTI9_5_IRQHandler, default_handler
    .weak TIM1_BRK_TIM9_IRQHandler
    .thumb_set TIM1_BRK_TIM9_IRQHandler, default_handler
    .weak TIM1_UP_TIM10_IRQHandler
    .thumb_set TIM1_UP_TIM10_IRQHandler, default_handler
    .weak TIM1_TRG_COM_TIM11_IRQHandler
    .thumb_set TIM1_TRG_COM_TIM11_IRQHandler, default_handler
    .weak TIM1_CC_IRQHandler
    .thumb_set TIM1_CC_IRQHandler, default_handler
    .weak TIM2_IRQHandler
    .thumb_set TIM2_IRQHandler, default_handler
    .weak TIM3_IRQHandler
    .thumb_set TIM3_IRQHandler, default_handler
    .weak TIM4_IRQHandler
    .thumb_set TIM4_IRQHandler, default_handler
    .weak I2C1_EV_IRQHandler
    .thumb_set I2C1_EV_IRQHandler, default_handler
    .weak I2C1_ER_IRQHandler
    .thumb_set I2C1_ER_IRQHandler, default_handler
    .weak I2C2_EV_IRQHandler
    .thumb_set I2C2_EV_IRQHandler, default_handler
    .weak I2C2_ER_IRQHandler
    .thumb_set I2C2_ER_IRQHandler, default_handler
    .weak SPI1_IRQHandler
    .thumb_set SPI1_IRQHandler, default_handler
    .weak SPI2_IRQHandler
    .thumb_set SPI2_IRQHandler, default_handler
    .weak USART1_IRQHandler
    .thumb_set USART1_IRQHandler, default_handler
    .weak USART2_IRQHandler
    .thumb_set USART2_IRQHandler, default_handler
    .weak EXTI15_10_IRQHandler
    .thumb_set EXTI15_10_IRQHandler, default_handler
    .weak RTC_Alarm_IRQHandler
    .thumb_set RTC_Alarm_IRQHandler, default_handler
    .weak OTG_FS_WKUP_IRQHandler
    .thumb_set OTG_FS_WKUP_IRQHandler, default_handler
    .weak DMA1_Stream7_IRQHandler
    .thumb_set DMA1_Stream7_IRQHandler, default_handler
    .weak SDIO_IRQHandler
    .thumb_set SDIO_IRQHandler, default_handler
    .weak TIM5_IRQHandler
    .thumb_set TIM5_IRQHandler, default_handler
    .weak SPI3_IRQHandler
    .thumb_set SPI3_IRQHandler, default_handler
    .weak DMA2_Stream0_IRQHandler
    .thumb_set DMA2_Stream0_IRQHandler, default_handler
    .weak DMA2_Stream1_IRQHandler
    .thumb_set DMA2_Stream1_IRQHandler, default_handler
    .weak DMA2_Stream2_IRQHandler
    .thumb_set DMA2_Stream2_IRQHandler, default_handler
    .weak DMA2_Stream3_IRQHandler
    .thumb_set DMA2_Stream3_IRQHandler, default_handler
    .weak DMA2_Stream4_IRQHandler
    .thumb_set DMA2_Stream4_IRQHandler, default_handler
    .weak OTG_FS_IRQHandler
    .thumb_set OTG_FS_IRQHandler, default_handler
    .weak DMA2_Stream5_IRQHandler
    .thumb_set DMA2_Stream5_IRQHandler, default_handler
    .weak DMA2_Stream6_IRQHandler
    .thumb_set DMA2_Stream6_IRQHandler, default_handler
    .weak DMA2_Stream7_IRQHandler
    .thumb_set DMA2_Stream7_IRQHandler, default_handler
    .weak USART6_IRQHandler
    .thumb_set USART6_IRQHandler, default_handler
    .weak I2C3_EV_IRQHandler
    .thumb_set I2C3_EV_IRQHandler, default_handler
    .weak I2C3_ER_IRQHandler
    .thumb_set I2C3_ER_IRQHandler, default_handler
    .weak FPU_IRQHandler
    .thumb_set FPU_IRQHandler, default_handler
    .weak SPI4_IRQHandler
    .thumb_set SPI4_IRQHandler, default_handler
