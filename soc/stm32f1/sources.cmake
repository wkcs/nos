# SoC-specific sources and private include paths.
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
