# I2C controller source selection.
if(CONFIG_I2C_GPIO)
    list(APPEND DRIVERS_SRCS i2c/i2c_gpio.c)
endif()
