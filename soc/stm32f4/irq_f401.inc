/* Copyright (C) 2026 Nick Hu. STM32F401 IRQ numbers follow the vendor CMSIS header. */
    .word WWDG_IRQHandler /* IRQ 0 */
    .word PVD_IRQHandler /* IRQ 1 */
    .word TAMP_STAMP_IRQHandler /* IRQ 2 */
    .word RTC_WKUP_IRQHandler /* IRQ 3 */
    .word FLASH_IRQHandler /* IRQ 4 */
    .word RCC_IRQHandler /* IRQ 5 */
    .word EXTI0_IRQHandler /* IRQ 6 */
    .word EXTI1_IRQHandler /* IRQ 7 */
    .word EXTI2_IRQHandler /* IRQ 8 */
    .word EXTI3_IRQHandler /* IRQ 9 */
    .word EXTI4_IRQHandler /* IRQ 10 */
    .word DMA1_Stream0_IRQHandler /* IRQ 11 */
    .word DMA1_Stream1_IRQHandler /* IRQ 12 */
    .word DMA1_Stream2_IRQHandler /* IRQ 13 */
    .word DMA1_Stream3_IRQHandler /* IRQ 14 */
    .word DMA1_Stream4_IRQHandler /* IRQ 15 */
    .word DMA1_Stream5_IRQHandler /* IRQ 16 */
    .word DMA1_Stream6_IRQHandler /* IRQ 17 */
    .word ADC_IRQHandler /* IRQ 18 */
    .word 0 /* IRQ 19 */
    .word 0 /* IRQ 20 */
    .word 0 /* IRQ 21 */
    .word 0 /* IRQ 22 */
    .word EXTI9_5_IRQHandler /* IRQ 23 */
    .word TIM1_BRK_TIM9_IRQHandler /* IRQ 24 */
    .word TIM1_UP_TIM10_IRQHandler /* IRQ 25 */
    .word TIM1_TRG_COM_TIM11_IRQHandler /* IRQ 26 */
    .word TIM1_CC_IRQHandler /* IRQ 27 */
    .word TIM2_IRQHandler /* IRQ 28 */
    .word TIM3_IRQHandler /* IRQ 29 */
    .word TIM4_IRQHandler /* IRQ 30 */
    .word I2C1_EV_IRQHandler /* IRQ 31 */
    .word I2C1_ER_IRQHandler /* IRQ 32 */
    .word I2C2_EV_IRQHandler /* IRQ 33 */
    .word I2C2_ER_IRQHandler /* IRQ 34 */
    .word SPI1_IRQHandler /* IRQ 35 */
    .word SPI2_IRQHandler /* IRQ 36 */
    .word USART1_IRQHandler /* IRQ 37 */
    .word USART2_IRQHandler /* IRQ 38 */
    .word 0 /* IRQ 39 */
    .word EXTI15_10_IRQHandler /* IRQ 40 */
    .word RTC_Alarm_IRQHandler /* IRQ 41 */
    .word OTG_FS_WKUP_IRQHandler /* IRQ 42 */
    .word 0 /* IRQ 43 */
    .word 0 /* IRQ 44 */
    .word 0 /* IRQ 45 */
    .word 0 /* IRQ 46 */
    .word DMA1_Stream7_IRQHandler /* IRQ 47 */
    .word 0 /* IRQ 48 */
    .word SDIO_IRQHandler /* IRQ 49 */
    .word TIM5_IRQHandler /* IRQ 50 */
    .word SPI3_IRQHandler /* IRQ 51 */
    .word 0 /* IRQ 52 */
    .word 0 /* IRQ 53 */
    .word 0 /* IRQ 54 */
    .word 0 /* IRQ 55 */
    .word DMA2_Stream0_IRQHandler /* IRQ 56 */
    .word DMA2_Stream1_IRQHandler /* IRQ 57 */
    .word DMA2_Stream2_IRQHandler /* IRQ 58 */
    .word DMA2_Stream3_IRQHandler /* IRQ 59 */
    .word DMA2_Stream4_IRQHandler /* IRQ 60 */
    .word 0 /* IRQ 61 */
    .word 0 /* IRQ 62 */
    .word 0 /* IRQ 63 */
    .word 0 /* IRQ 64 */
    .word 0 /* IRQ 65 */
    .word 0 /* IRQ 66 */
    .word OTG_FS_IRQHandler /* IRQ 67 */
    .word DMA2_Stream5_IRQHandler /* IRQ 68 */
    .word DMA2_Stream6_IRQHandler /* IRQ 69 */
    .word DMA2_Stream7_IRQHandler /* IRQ 70 */
    .word USART6_IRQHandler /* IRQ 71 */
    .word I2C3_EV_IRQHandler /* IRQ 72 */
    .word I2C3_ER_IRQHandler /* IRQ 73 */
    .word 0 /* IRQ 74 */
    .word 0 /* IRQ 75 */
    .word 0 /* IRQ 76 */
    .word 0 /* IRQ 77 */
    .word 0 /* IRQ 78 */
    .word 0 /* IRQ 79 */
    .word 0 /* IRQ 80 */
    .word FPU_IRQHandler /* IRQ 81 */
    .word 0 /* IRQ 82 */
    .word 0 /* IRQ 83 */
    .word SPI4_IRQHandler /* IRQ 84 */
    .weak WWDG_IRQHandler
    .thumb_set WWDG_IRQHandler, Default_Handler
    .weak PVD_IRQHandler
    .thumb_set PVD_IRQHandler, Default_Handler
    .weak TAMP_STAMP_IRQHandler
    .thumb_set TAMP_STAMP_IRQHandler, Default_Handler
    .weak RTC_WKUP_IRQHandler
    .thumb_set RTC_WKUP_IRQHandler, Default_Handler
    .weak FLASH_IRQHandler
    .thumb_set FLASH_IRQHandler, Default_Handler
    .weak RCC_IRQHandler
    .thumb_set RCC_IRQHandler, Default_Handler
    .weak EXTI0_IRQHandler
    .thumb_set EXTI0_IRQHandler, Default_Handler
    .weak EXTI1_IRQHandler
    .thumb_set EXTI1_IRQHandler, Default_Handler
    .weak EXTI2_IRQHandler
    .thumb_set EXTI2_IRQHandler, Default_Handler
    .weak EXTI3_IRQHandler
    .thumb_set EXTI3_IRQHandler, Default_Handler
    .weak EXTI4_IRQHandler
    .thumb_set EXTI4_IRQHandler, Default_Handler
    .weak DMA1_Stream0_IRQHandler
    .thumb_set DMA1_Stream0_IRQHandler, Default_Handler
    .weak DMA1_Stream1_IRQHandler
    .thumb_set DMA1_Stream1_IRQHandler, Default_Handler
    .weak DMA1_Stream2_IRQHandler
    .thumb_set DMA1_Stream2_IRQHandler, Default_Handler
    .weak DMA1_Stream3_IRQHandler
    .thumb_set DMA1_Stream3_IRQHandler, Default_Handler
    .weak DMA1_Stream4_IRQHandler
    .thumb_set DMA1_Stream4_IRQHandler, Default_Handler
    .weak DMA1_Stream5_IRQHandler
    .thumb_set DMA1_Stream5_IRQHandler, Default_Handler
    .weak DMA1_Stream6_IRQHandler
    .thumb_set DMA1_Stream6_IRQHandler, Default_Handler
    .weak ADC_IRQHandler
    .thumb_set ADC_IRQHandler, Default_Handler
    .weak EXTI9_5_IRQHandler
    .thumb_set EXTI9_5_IRQHandler, Default_Handler
    .weak TIM1_BRK_TIM9_IRQHandler
    .thumb_set TIM1_BRK_TIM9_IRQHandler, Default_Handler
    .weak TIM1_UP_TIM10_IRQHandler
    .thumb_set TIM1_UP_TIM10_IRQHandler, Default_Handler
    .weak TIM1_TRG_COM_TIM11_IRQHandler
    .thumb_set TIM1_TRG_COM_TIM11_IRQHandler, Default_Handler
    .weak TIM1_CC_IRQHandler
    .thumb_set TIM1_CC_IRQHandler, Default_Handler
    .weak TIM2_IRQHandler
    .thumb_set TIM2_IRQHandler, Default_Handler
    .weak TIM3_IRQHandler
    .thumb_set TIM3_IRQHandler, Default_Handler
    .weak TIM4_IRQHandler
    .thumb_set TIM4_IRQHandler, Default_Handler
    .weak I2C1_EV_IRQHandler
    .thumb_set I2C1_EV_IRQHandler, Default_Handler
    .weak I2C1_ER_IRQHandler
    .thumb_set I2C1_ER_IRQHandler, Default_Handler
    .weak I2C2_EV_IRQHandler
    .thumb_set I2C2_EV_IRQHandler, Default_Handler
    .weak I2C2_ER_IRQHandler
    .thumb_set I2C2_ER_IRQHandler, Default_Handler
    .weak SPI1_IRQHandler
    .thumb_set SPI1_IRQHandler, Default_Handler
    .weak SPI2_IRQHandler
    .thumb_set SPI2_IRQHandler, Default_Handler
    .weak USART1_IRQHandler
    .thumb_set USART1_IRQHandler, Default_Handler
    .weak USART2_IRQHandler
    .thumb_set USART2_IRQHandler, Default_Handler
    .weak EXTI15_10_IRQHandler
    .thumb_set EXTI15_10_IRQHandler, Default_Handler
    .weak RTC_Alarm_IRQHandler
    .thumb_set RTC_Alarm_IRQHandler, Default_Handler
    .weak OTG_FS_WKUP_IRQHandler
    .thumb_set OTG_FS_WKUP_IRQHandler, Default_Handler
    .weak DMA1_Stream7_IRQHandler
    .thumb_set DMA1_Stream7_IRQHandler, Default_Handler
    .weak SDIO_IRQHandler
    .thumb_set SDIO_IRQHandler, Default_Handler
    .weak TIM5_IRQHandler
    .thumb_set TIM5_IRQHandler, Default_Handler
    .weak SPI3_IRQHandler
    .thumb_set SPI3_IRQHandler, Default_Handler
    .weak DMA2_Stream0_IRQHandler
    .thumb_set DMA2_Stream0_IRQHandler, Default_Handler
    .weak DMA2_Stream1_IRQHandler
    .thumb_set DMA2_Stream1_IRQHandler, Default_Handler
    .weak DMA2_Stream2_IRQHandler
    .thumb_set DMA2_Stream2_IRQHandler, Default_Handler
    .weak DMA2_Stream3_IRQHandler
    .thumb_set DMA2_Stream3_IRQHandler, Default_Handler
    .weak DMA2_Stream4_IRQHandler
    .thumb_set DMA2_Stream4_IRQHandler, Default_Handler
    .weak OTG_FS_IRQHandler
    .thumb_set OTG_FS_IRQHandler, Default_Handler
    .weak DMA2_Stream5_IRQHandler
    .thumb_set DMA2_Stream5_IRQHandler, Default_Handler
    .weak DMA2_Stream6_IRQHandler
    .thumb_set DMA2_Stream6_IRQHandler, Default_Handler
    .weak DMA2_Stream7_IRQHandler
    .thumb_set DMA2_Stream7_IRQHandler, Default_Handler
    .weak USART6_IRQHandler
    .thumb_set USART6_IRQHandler, Default_Handler
    .weak I2C3_EV_IRQHandler
    .thumb_set I2C3_EV_IRQHandler, Default_Handler
    .weak I2C3_ER_IRQHandler
    .thumb_set I2C3_ER_IRQHandler, Default_Handler
    .weak FPU_IRQHandler
    .thumb_set FPU_IRQHandler, Default_Handler
    .weak SPI4_IRQHandler
    .thumb_set SPI4_IRQHandler, Default_Handler
