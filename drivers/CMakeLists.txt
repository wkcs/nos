set(DRIVERS_SRCS
    base/device.c
    base/null_device.c
)

if(CONFIG_RAMDISK)
    list(APPEND DRIVERS_SRCS block/ramdisk.c)
endif()

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

if(CONFIG_I2C_CORE)
    list(APPEND DRIVERS_SRCS i2c/i2c-core.c)
endif()

if(CONFIG_NK60_V1_KEY)
    list(APPEND DRIVERS_SRCS key/keyboard.c)
endif()
if(CONFIG_NK60_V2_KEY)
    list(APPEND DRIVERS_SRCS key/nk60_v2_key.c)
endif()

if(CONFIG_STM32PWM)
    list(APPEND DRIVERS_SRCS misc/stm32_pwm.c)
endif()
if(CONFIG_NK60_V2_LED)
    list(APPEND DRIVERS_SRCS misc/nk60_v2_led.c)
endif()

if(CONFIG_SOC_STM32F1 OR CONFIG_SOC_STM32F4)
    list(APPEND DRIVERS_SRCS serial/uart_stm32.c serial/console_uart.c)
elseif(CONFIG_SOC_NUC472)
    list(APPEND DRIVERS_SRCS serial/uart_nuc472.c serial/console_uart.c)
elseif(CONFIG_ARCH_ARM64 AND CONFIG_BOARD_QEMU_VIRT)
    list(APPEND DRIVERS_SRCS serial/uart_pl011.c serial/console_uart.c)
endif()

if(CONFIG_SPI_595)
    list(APPEND DRIVERS_SRCS spi/spi-74hc595.c)
endif()
if(CONFIG_NK60_V2_W25QXX)
    list(APPEND DRIVERS_SRCS spi/nk60_v2-w25qxx.c flash/w25q_spi.c)
endif()
if(CONFIG_SPI_SDCARD)
    list(APPEND DRIVERS_SRCS spi/spi-sdcard.c)
endif()
if(CONFIG_QEMU_SD)
    list(APPEND DRIVERS_SRCS spi/qemu_sd.c)
endif()

if(CONFIG_USB)
    list(APPEND DRIVERS_SRCS
        usb/device/core/core.c
        usb/device/core/usbdevice.c
    )
    if(CONFIG_USB_HID)
        list(APPEND DRIVERS_SRCS usb/device/class/hid.c)
    endif()
    if(CONFIG_USB_WINUSB)
        list(APPEND DRIVERS_SRCS usb/device/class/winusb.c)
    endif()
    if(CONFIG_USB_CDC)
        list(APPEND DRIVERS_SRCS usb/device/class/cdc.c)
    endif()
endif()

add_library(nos_drivers OBJECT ${DRIVERS_SRCS})
nos_configure_kernel_target(nos_drivers)
