# Sources selected for this driver class.
if(CONFIG_SOC_STM32F1 OR CONFIG_SOC_STM32F4)
    list(APPEND DRIVERS_SRCS serial/uart_stm32.c serial/console_uart.c)
elseif(CONFIG_SOC_NUC472)
    list(APPEND DRIVERS_SRCS serial/uart_nuc472.c serial/console_uart.c)
elseif(CONFIG_ARCH_ARM64 AND CONFIG_BOARD_QEMU_VIRT)
    list(APPEND DRIVERS_SRCS serial/uart_pl011.c serial/console_uart.c)
endif()

