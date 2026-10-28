# QEMU ARM64 currently supplies a board-specific initialization routine.
if(CONFIG_ARCH_ARM64)
    list(APPEND BOARD_SRCS ${BOARD_DIR}/init.c)
endif()
