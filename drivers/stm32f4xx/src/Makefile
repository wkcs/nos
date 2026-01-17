#
# STM32F4xx 标准外设库源文件 Makefile
#

# 基础驱动
obj-y += misc.o
obj-y += stm32f4xx_rcc.o
obj-y += stm32f4xx_gpio.o
obj-y += stm32f4xx_exti.o
obj-y += stm32f4xx_syscfg.o

# 通信接口
obj-$(CONFIG_STM32F4XX_USART) += stm32f4xx_usart.o
obj-$(CONFIG_STM32F4XX_SPI) += stm32f4xx_spi.o
obj-$(CONFIG_STM32F4XX_I2C) += stm32f4xx_i2c.o
obj-$(CONFIG_STM32F4XX_CAN) += stm32f4xx_can.o

# 定时器
obj-$(CONFIG_STM32F4XX_TIM) += stm32f4xx_tim.o

# ADC/DAC
obj-$(CONFIG_STM32F4XX_ADC) += stm32f4xx_adc.o
obj-$(CONFIG_STM32F4XX_DAC) += stm32f4xx_dac.o

# DMA
obj-$(CONFIG_STM32F4XX_DMA) += stm32f4xx_dma.o
obj-$(CONFIG_STM32F4XX_DMA2D) += stm32f4xx_dma2d.o

# 存储
obj-$(CONFIG_STM32F4XX_FLASH) += stm32f4xx_flash.o
obj-$(CONFIG_STM32F4XX_FLASH_RAMFUNC) += stm32f4xx_flash_ramfunc.o
obj-$(CONFIG_STM32F4XX_FSMC) += stm32f4xx_fsmc.o
obj-$(CONFIG_STM32F4XX_SDIO) += stm32f4xx_sdio.o

# 电源管理
obj-$(CONFIG_STM32F4XX_PWR) += stm32f4xx_pwr.o
obj-$(CONFIG_STM32F4XX_RTC) += stm32f4xx_rtc.o

# 看门狗
obj-$(CONFIG_STM32F4XX_IWDG) += stm32f4xx_iwdg.o
obj-$(CONFIG_STM32F4XX_WWDG) += stm32f4xx_wwdg.o

# 加密和哈希
obj-$(CONFIG_STM32F4XX_CRYP) += stm32f4xx_cryp.o
obj-$(CONFIG_STM32F4XX_CRYP) += stm32f4xx_cryp_aes.o
obj-$(CONFIG_STM32F4XX_CRYP) += stm32f4xx_cryp_des.o
obj-$(CONFIG_STM32F4XX_CRYP) += stm32f4xx_cryp_tdes.o
obj-$(CONFIG_STM32F4XX_HASH) += stm32f4xx_hash.o
obj-$(CONFIG_STM32F4XX_HASH) += stm32f4xx_hash_md5.o
obj-$(CONFIG_STM32F4XX_HASH) += stm32f4xx_hash_sha1.o

# 随机数生成器
obj-$(CONFIG_STM32F4XX_RNG) += stm32f4xx_rng.o

# 显示相关
obj-$(CONFIG_STM32F4XX_LTDC) += stm32f4xx_ltdc.o
obj-$(CONFIG_STM32F4XX_DCMI) += stm32f4xx_dcmi.o

# 音频
obj-$(CONFIG_STM32F4XX_SAI) += stm32f4xx_sai.o

# CRC
obj-$(CONFIG_STM32F4XX_CRC) += stm32f4xx_crc.o

# 调试
obj-$(CONFIG_STM32F4XX_DBGMCU) += stm32f4xx_dbgmcu.o