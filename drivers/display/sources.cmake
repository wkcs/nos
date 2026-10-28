# Sources selected for this driver class.
if(CONFIG_RGB_MATRIX)
    list(APPEND DRIVERS_SRCS display/rgb-matrix.c)
endif()
if(CONFIG_ZJ_TFTLCD)
    list(APPEND DRIVERS_SRCS display/zj-tft-lcd.c)
endif()
if(CONFIG_ST7789_LCD)
    list(APPEND DRIVERS_SRCS display/st7789-lcd.c)
endif()
if(CONFIG_SSD1106_OLED)
    list(APPEND DRIVERS_SRCS display/ssd1106-oled.c)
endif()

