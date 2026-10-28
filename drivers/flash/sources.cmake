# Flash chip protocol implementations.
if(CONFIG_NK60_V2_W25QXX)
    list(APPEND DRIVERS_SRCS flash/w25q_spi.c flash/w25q_flash.c)
    if(CONFIG_DEVICE_TREE)
        list(APPEND DRIVERS_SRCS flash/w25q_dt.c)
    endif()
endif()
