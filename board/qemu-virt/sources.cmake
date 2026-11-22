# QEMU Virt board initialization sources
list(APPEND BOARD_SRCS ${BOARD_DIR}/init.c)
if(CONFIG_ARCH_ARM64)
    list(APPEND BOARD_SRCS ${BOARD_DIR}/board_mm.c)
endif()
