# Sources selected for this driver class.
if(CONFIG_SPI_595)
    list(APPEND DRIVERS_SRCS spi/spi-74hc595.c)
endif()
if(CONFIG_NK60_V2_W25QXX)
    list(APPEND DRIVERS_SRCS spi/nk60_v2-w25qxx.c)
endif()
if(CONFIG_SPI_SDCARD)
    list(APPEND DRIVERS_SRCS spi/spi-sdcard.c)
endif()
if(CONFIG_QEMU_SD)
    list(APPEND DRIVERS_SRCS spi/qemu_sd.c)
endif()

