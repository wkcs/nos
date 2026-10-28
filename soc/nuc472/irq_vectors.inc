/*
 * Copyright (C) 2019 胡启航<Hu Qihang>
 *
 * Author: wkcs
 *
 * Email: hqh2030@gmail.com, huqihan@live.com
 */

/* NUC472 external interrupts; USB occupies IRQs 88-90. */
    .rept 88
    .word Default_Handler
    .endr
    .word USBD_IRQHandler
    .word USBH_IRQHandler
    .word USB_OTG_IRQHandler
    .rept 51
    .word Default_Handler
    .endr
    .weak USBD_IRQHandler
    .thumb_set USBD_IRQHandler, Default_Handler
    .weak USBH_IRQHandler
    .thumb_set USBH_IRQHandler, Default_Handler
    .weak USB_OTG_IRQHandler
    .thumb_set USB_OTG_IRQHandler, Default_Handler
