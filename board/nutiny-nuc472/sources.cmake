# Board-specific sources and private include paths.
list(APPEND BOARD_SRCS ${BOARD_DIR}/init.c ${BOARD_DIR}/led.c)

if(CONFIG_LITTLEFS)
    list(APPEND BOARD_SRCS ${BOARD_DIR}/board_fs.c)
endif()

if(CONFIG_NLK_SERVER)
    list(APPEND BOARD_SRCS ${BOARD_DIR}/board_nlk.c)
endif()

if(CONFIG_SHELL)
    list(APPEND BOARD_SRCS ${BOARD_DIR}/board_cmd.c)
endif()

if(CONFIG_USB)
    list(APPEND BOARD_SRCS ${BOARD_DIR}/arch_usb.c)
endif()
