# Sources selected for this driver class.
list(APPEND DRIVERS_SRCS
    base/device.c
    base/null_device.c
)

if(CONFIG_DEVICE_TREE)
    list(APPEND DRIVERS_SRCS base/devicetree.c "${NOS_DTS_C}")
endif()

