# Sources selected for this driver class.
if(CONFIG_SPI_595)
    list(APPEND DRIVERS_SRCS spi/spi-74hc595.c)
endif()
if(CONFIG_STM32F4_SPI OR CONFIG_STM32F1_SPI)
    list(APPEND DRIVERS_SRCS spi/spi_stm32.c)
endif()
if(CONFIG_SPI_SDCARD)
    list(APPEND DRIVERS_SRCS spi/spi-sdcard.c block/sd_spi.c block/sd_spi_card.c block/sd_spi_io.c)
endif()
if(CONFIG_QEMU_SD)
    list(APPEND DRIVERS_SRCS spi/qemu_sd.c)
endif()

