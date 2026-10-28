# Sources selected for this driver class.
if(CONFIG_NK60_V1_KEY)
    list(APPEND DRIVERS_SRCS key/keyboard.c)
endif()
if(CONFIG_NK60_V2_KEY)
    list(APPEND DRIVERS_SRCS key/nk60_v2_key.c)
endif()

