# Sources selected for this driver class.
if(CONFIG_STM32PWM)
    list(APPEND DRIVERS_SRCS misc/stm32_pwm.c)
endif()
if(CONFIG_NK60_V2_LED)
    list(APPEND DRIVERS_SRCS misc/nk60_v2_led.c)
endif()

