/*
 * Copyright (C) 2019 胡启航<Hu Qihang>
 *
 * Author: wkcs
 *
 * Email: hqh2030@gmail.com, huqihan@live.com
 */

/* STM32F10x medium-density IRQs 0-42. */
    .word WWDG_IRQHandler /* IRQ 0 */
    .word PVD_IRQHandler /* IRQ 1 */
    .word TAMPER_IRQHandler /* IRQ 2 */
    .word RTC_IRQHandler /* IRQ 3 */
    .word FLASH_IRQHandler /* IRQ 4 */
    .word RCC_IRQHandler /* IRQ 5 */
    .word EXTI0_IRQHandler /* IRQ 6 */
    .word EXTI1_IRQHandler /* IRQ 7 */
    .word EXTI2_IRQHandler /* IRQ 8 */
    .word EXTI3_IRQHandler /* IRQ 9 */
    .word EXTI4_IRQHandler /* IRQ 10 */
    .word DMA1_Channel1_IRQHandler /* IRQ 11 */
    .word DMA1_Channel2_IRQHandler /* IRQ 12 */
    .word DMA1_Channel3_IRQHandler /* IRQ 13 */
    .word DMA1_Channel4_IRQHandler /* IRQ 14 */
    .word DMA1_Channel5_IRQHandler /* IRQ 15 */
    .word DMA1_Channel6_IRQHandler /* IRQ 16 */
    .word DMA1_Channel7_IRQHandler /* IRQ 17 */
    .word ADC1_2_IRQHandler /* IRQ 18 */
    .word USB_HP_CAN1_TX_IRQHandler /* IRQ 19 */
    .word USB_LP_CAN1_RX0_IRQHandler /* IRQ 20 */
    .word CAN1_RX1_IRQHandler /* IRQ 21 */
    .word CAN1_SCE_IRQHandler /* IRQ 22 */
    .word EXTI9_5_IRQHandler /* IRQ 23 */
    .word TIM1_BRK_IRQHandler /* IRQ 24 */
    .word TIM1_UP_IRQHandler /* IRQ 25 */
    .word TIM1_TRG_COM_IRQHandler /* IRQ 26 */
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
    .word USART3_IRQHandler /* IRQ 39 */
    .word EXTI15_10_IRQHandler /* IRQ 40 */
    .word RTCAlarm_IRQHandler /* IRQ 41 */
    .word USBWakeUp_IRQHandler /* IRQ 42 */
#ifdef CONFIG_SOC_STM32F1_HD
    .word TIM8_BRK_IRQHandler /* IRQ 43 */
    .word TIM8_UP_IRQHandler /* IRQ 44 */
    .word TIM8_TRG_COM_IRQHandler /* IRQ 45 */
    .word TIM8_CC_IRQHandler /* IRQ 46 */
    .word ADC3_IRQHandler /* IRQ 47 */
    .word FSMC_IRQHandler /* IRQ 48 */
    .word SDIO_IRQHandler /* IRQ 49 */
    .word TIM5_IRQHandler /* IRQ 50 */
    .word SPI3_IRQHandler /* IRQ 51 */
    .word UART4_IRQHandler /* IRQ 52 */
    .word UART5_IRQHandler /* IRQ 53 */
    .word TIM6_IRQHandler /* IRQ 54 */
    .word TIM7_IRQHandler /* IRQ 55 */
    .word DMA2_Channel1_IRQHandler /* IRQ 56 */
    .word DMA2_Channel2_IRQHandler /* IRQ 57 */
    .word DMA2_Channel3_IRQHandler /* IRQ 58 */
    .word DMA2_Channel4_5_IRQHandler /* IRQ 59 */
#endif
    .weak WWDG_IRQHandler
    .thumb_set WWDG_IRQHandler, Default_Handler
    .weak PVD_IRQHandler
    .thumb_set PVD_IRQHandler, Default_Handler
    .weak TAMPER_IRQHandler
    .thumb_set TAMPER_IRQHandler, Default_Handler
    .weak RTC_IRQHandler
    .thumb_set RTC_IRQHandler, Default_Handler
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
    .weak DMA1_Channel1_IRQHandler
    .thumb_set DMA1_Channel1_IRQHandler, Default_Handler
    .weak DMA1_Channel2_IRQHandler
    .thumb_set DMA1_Channel2_IRQHandler, Default_Handler
    .weak DMA1_Channel3_IRQHandler
    .thumb_set DMA1_Channel3_IRQHandler, Default_Handler
    .weak DMA1_Channel4_IRQHandler
    .thumb_set DMA1_Channel4_IRQHandler, Default_Handler
    .weak DMA1_Channel5_IRQHandler
    .thumb_set DMA1_Channel5_IRQHandler, Default_Handler
    .weak DMA1_Channel6_IRQHandler
    .thumb_set DMA1_Channel6_IRQHandler, Default_Handler
    .weak DMA1_Channel7_IRQHandler
    .thumb_set DMA1_Channel7_IRQHandler, Default_Handler
    .weak ADC1_2_IRQHandler
    .thumb_set ADC1_2_IRQHandler, Default_Handler
    .weak USB_HP_CAN1_TX_IRQHandler
    .thumb_set USB_HP_CAN1_TX_IRQHandler, Default_Handler
    .weak USB_LP_CAN1_RX0_IRQHandler
    .thumb_set USB_LP_CAN1_RX0_IRQHandler, Default_Handler
    .weak CAN1_RX1_IRQHandler
    .thumb_set CAN1_RX1_IRQHandler, Default_Handler
    .weak CAN1_SCE_IRQHandler
    .thumb_set CAN1_SCE_IRQHandler, Default_Handler
    .weak EXTI9_5_IRQHandler
    .thumb_set EXTI9_5_IRQHandler, Default_Handler
    .weak TIM1_BRK_IRQHandler
    .thumb_set TIM1_BRK_IRQHandler, Default_Handler
    .weak TIM1_UP_IRQHandler
    .thumb_set TIM1_UP_IRQHandler, Default_Handler
    .weak TIM1_TRG_COM_IRQHandler
    .thumb_set TIM1_TRG_COM_IRQHandler, Default_Handler
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
    .weak USART3_IRQHandler
    .thumb_set USART3_IRQHandler, Default_Handler
    .weak EXTI15_10_IRQHandler
    .thumb_set EXTI15_10_IRQHandler, Default_Handler
    .weak RTCAlarm_IRQHandler
    .thumb_set RTCAlarm_IRQHandler, Default_Handler
    .weak USBWakeUp_IRQHandler
    .thumb_set USBWakeUp_IRQHandler, Default_Handler
#ifdef CONFIG_SOC_STM32F1_HD
    .weak TIM8_BRK_IRQHandler
    .thumb_set TIM8_BRK_IRQHandler, Default_Handler
    .weak TIM8_UP_IRQHandler
    .thumb_set TIM8_UP_IRQHandler, Default_Handler
    .weak TIM8_TRG_COM_IRQHandler
    .thumb_set TIM8_TRG_COM_IRQHandler, Default_Handler
    .weak TIM8_CC_IRQHandler
    .thumb_set TIM8_CC_IRQHandler, Default_Handler
    .weak ADC3_IRQHandler
    .thumb_set ADC3_IRQHandler, Default_Handler
    .weak FSMC_IRQHandler
    .thumb_set FSMC_IRQHandler, Default_Handler
    .weak SDIO_IRQHandler
    .thumb_set SDIO_IRQHandler, Default_Handler
    .weak TIM5_IRQHandler
    .thumb_set TIM5_IRQHandler, Default_Handler
    .weak SPI3_IRQHandler
    .thumb_set SPI3_IRQHandler, Default_Handler
    .weak UART4_IRQHandler
    .thumb_set UART4_IRQHandler, Default_Handler
    .weak UART5_IRQHandler
    .thumb_set UART5_IRQHandler, Default_Handler
    .weak TIM6_IRQHandler
    .thumb_set TIM6_IRQHandler, Default_Handler
    .weak TIM7_IRQHandler
    .thumb_set TIM7_IRQHandler, Default_Handler
    .weak DMA2_Channel1_IRQHandler
    .thumb_set DMA2_Channel1_IRQHandler, Default_Handler
    .weak DMA2_Channel2_IRQHandler
    .thumb_set DMA2_Channel2_IRQHandler, Default_Handler
    .weak DMA2_Channel3_IRQHandler
    .thumb_set DMA2_Channel3_IRQHandler, Default_Handler
    .weak DMA2_Channel4_5_IRQHandler
    .thumb_set DMA2_Channel4_5_IRQHandler, Default_Handler
#endif
