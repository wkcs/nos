if(CONFIG_NOS_AI_CORE)
    list(APPEND DRIVERS_SRCS
        accel/virtio_accel.c
        accel/rknpu.c
    )
endif()
