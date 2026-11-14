# NUC472 GPIO 与 EXTI 驱动
if(CONFIG_NUC472_GPIO)
    list(APPEND DRIVERS_SRCS gpio/gpio_nuc472.c)
endif()
