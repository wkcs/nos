# Sources selected for this driver class.
if(CONFIG_RAMDISK)
    list(APPEND DRIVERS_SRCS block/ramdisk.c)
endif()
if(CONFIG_SPI_SDCARD)
    list(APPEND DRIVERS_SRCS block/sd_block.c)
endif()

