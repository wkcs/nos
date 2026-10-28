# Sources selected for this driver class.
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

