# Board-specific sources and private include paths.
list(APPEND BOARD_SRCS
    ${BOARD_DIR}/init.c
    ${BOARD_DIR}/board_config.c
    ${BOARD_DIR}/board_mm.c
    ${BOARD_DIR}/arch_dma.c
    ${BOARD_DIR}/arch_clk.c
    ${BOARD_DIR}/arch_irq.c
    ${BOARD_DIR}/arch_gpio.c
    ${BOARD_DIR}/arch_boot.c
)
list(APPEND BOARD_PRIVATE_INCLUDES
    "${BOARD_DIR}/inside-include"
    "${BOARD_DIR}/usb"
)
if(CONFIG_USB)
    list(APPEND BOARD_SRCS
        ${BOARD_DIR}/arch_usb.c
        ${BOARD_DIR}/usb/stm32f1xx_hal_pcd_ex.c
        ${BOARD_DIR}/usb/stm32f1xx_hal_pcd.c
        ${BOARD_DIR}/usb/stm32f1xx_ll_usb.c
    )
endif()
