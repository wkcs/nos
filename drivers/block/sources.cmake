# Sources selected for this driver class.
if(CONFIG_RAMDISK)
    list(APPEND DRIVERS_SRCS block/ramdisk.c)
endif()

