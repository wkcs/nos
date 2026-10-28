# SoC-specific sources and private include paths.
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

list(APPEND SOC_SRCS stm32f4/early_init.c)
