# Flash chip protocol implementations.
if(CONFIG_NK60_V2_W25QXX OR CONFIG_NK60_V1_W25QXX)
    list(APPEND DRIVERS_SRCS flash/w25q_spi.c flash/w25q_flash.c)
endif()

if(CONFIG_FLASH_NUC472_FMC)
    list(APPEND DRIVERS_SRCS flash/fmc_nuc472.c)
endif()
