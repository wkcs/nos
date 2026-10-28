set(SOC_SRCS "")
set(SOC_PRIVATE_INCLUDES "")

if(CONFIG_SOC_STM32F1)
    list(APPEND SOC_SRCS
        stm32f1/system_stm32f10x.c
        stm32f1/drivers/src/misc.c
        stm32f1/drivers/src/stm32f10x_adc.c
        stm32f1/drivers/src/stm32f10x_bkp.c
        stm32f1/drivers/src/stm32f10x_can.c
        stm32f1/drivers/src/stm32f10x_cec.c
        stm32f1/drivers/src/stm32f10x_crc.c
        stm32f1/drivers/src/stm32f10x_dac.c
        stm32f1/drivers/src/stm32f10x_dbgmcu.c
        stm32f1/drivers/src/stm32f10x_dma.c
        stm32f1/drivers/src/stm32f10x_exti.c
        stm32f1/drivers/src/stm32f10x_flash.c
        stm32f1/drivers/src/stm32f10x_fsmc.c
        stm32f1/drivers/src/stm32f10x_gpio.c
        stm32f1/drivers/src/stm32f10x_i2c.c
        stm32f1/drivers/src/stm32f10x_iwdg.c
        stm32f1/drivers/src/stm32f10x_pwr.c
        stm32f1/drivers/src/stm32f10x_rcc.c
        stm32f1/drivers/src/stm32f10x_rtc.c
        stm32f1/drivers/src/stm32f10x_sdio.c
        stm32f1/drivers/src/stm32f10x_spi.c
        stm32f1/drivers/src/stm32f10x_tim.c
        stm32f1/drivers/src/stm32f10x_usart.c
        stm32f1/drivers/src/stm32f10x_wwdg.c
    )
    list(APPEND SOC_PRIVATE_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/stm32f1/inside-include"
        "${CMAKE_CURRENT_SOURCE_DIR}/stm32f1/drivers/inc"
    )
endif()

if(CONFIG_SOC_STM32F4)
    list(APPEND SOC_SRCS
        stm32f4/system_stm32f4xx.c
        stm32f4/drivers/src/misc.c
        stm32f4/drivers/src/stm32f4xx_dbgmcu.c
        stm32f4/drivers/src/stm32f4xx_dma.c
        stm32f4/drivers/src/stm32f4xx_dma2d.c
        stm32f4/drivers/src/stm32f4xx_exti.c
        stm32f4/drivers/src/stm32f4xx_gpio.c
        stm32f4/drivers/src/stm32f4xx_i2c.c
        stm32f4/drivers/src/stm32f4xx_pwr.c
        stm32f4/drivers/src/stm32f4xx_rcc.c
        stm32f4/drivers/src/stm32f4xx_rtc.c
        stm32f4/drivers/src/stm32f4xx_spi.c
        stm32f4/drivers/src/stm32f4xx_syscfg.c
        stm32f4/drivers/src/stm32f4xx_tim.c
        stm32f4/drivers/src/stm32f4xx_usart.c
    )
    list(APPEND SOC_PRIVATE_INCLUDES
        "${CMAKE_CURRENT_SOURCE_DIR}/stm32f4/inside-include"
        "${CMAKE_CURRENT_SOURCE_DIR}/stm32f4/drivers/inc"
    )
endif()

if(CONFIG_SOC_NUC472)
    list(APPEND SOC_SRCS nuc472/system.c)
endif()

if(SOC_SRCS)
    add_library(nos_soc OBJECT ${SOC_SRCS})
    target_include_directories(nos_soc PRIVATE ${SOC_PRIVATE_INCLUDES})
    nos_configure_kernel_target(nos_soc)
else()
    add_library(nos_soc INTERFACE)
endif()
