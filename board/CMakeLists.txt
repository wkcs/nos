set(BOARD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/${CONFIG_BOARD}")
set(BOARD_SRCS "")
set(BOARD_PRIVATE_INCLUDES "")

if("${CONFIG_BOARD}" STREQUAL "nk60-v2")
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
            ${BOARD_DIR}/usb/stm32f4xx_hal_pcd_ex.c
            ${BOARD_DIR}/usb/stm32f4xx_hal_pcd.c
            ${BOARD_DIR}/usb/stm32f4xx_ll_usb.c
        )
    endif()
elseif("${CONFIG_BOARD}" STREQUAL "qemu-stm32")
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
elseif("${CONFIG_BOARD}" STREQUAL "wl-001" OR "${CONFIG_BOARD}" STREQUAL "zj-v3")
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
elseif("${CONFIG_BOARD}" STREQUAL "qemu-virt" AND CONFIG_ARCH_ARM64)
    list(APPEND BOARD_SRCS ${BOARD_DIR}/init.c)
endif()

if(BOARD_SRCS)
    add_library(nos_board OBJECT ${BOARD_SRCS})
    target_include_directories(nos_board PRIVATE ${BOARD_PRIVATE_INCLUDES})
    nos_configure_kernel_target(nos_board)
else()
    add_library(nos_board INTERFACE)
endif()
