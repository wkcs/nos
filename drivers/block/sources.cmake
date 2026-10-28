# Sources selected for this driver class.
if(CONFIG_RAMDISK)
    list(APPEND DRIVERS_SRCS block/ramdisk.c)
endif()
if(CONFIG_BLOCK_SPI_SD)
    list(APPEND DRIVERS_SRCS block/sd_block.c block/sd_spi.c block/sd_spi_card.c
                            block/sd_spi_io.c block/sd_csd.c)
endif()

