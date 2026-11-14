/*
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 外部中断向量表 (IRQ 0..141，共 142 项)
 */

    .word BOD_IRQHandler         /* 0 */
    .word IRC_IRQHandler         /* 1 */
    .word PWRWU_IRQHandler       /* 2 */
    .word RAMPE_IRQHandler       /* 3 */
    .word CKFAIL_IRQHandler      /* 4 */
    .word Default_Handler        /* 5: 保留 */
    .word RTC_IRQHandler         /* 6 */
    .word TAMPER_IRQHandler      /* 7 */
    .word EINT0_IRQHandler       /* 8 */
    .word EINT1_IRQHandler       /* 9 */
    .word EINT2_IRQHandler       /* 10 */
    .word EINT3_IRQHandler       /* 11 */
    .word EINT4_IRQHandler       /* 12 */
    .word EINT5_IRQHandler       /* 13 */
    .word EINT6_IRQHandler       /* 14 */
    .word EINT7_IRQHandler       /* 15 */
    .word GPA_IRQHandler         /* 16 */
    .word GPB_IRQHandler         /* 17 */
    .word GPC_IRQHandler         /* 18 */
    .word GPD_IRQHandler         /* 19 */
    .word GPE_IRQHandler         /* 20 */
    .word GPF_IRQHandler         /* 21 */
    .word GPG_IRQHandler         /* 22 */
    .word GPH_IRQHandler         /* 23 */
    .word GPI_IRQHandler         /* 24 */
    .word Default_Handler        /* 25: 保留 */
    .word Default_Handler        /* 26: 保留 */
    .word Default_Handler        /* 27: 保留 */
    .word Default_Handler        /* 28: 保留 */
    .word Default_Handler        /* 29: 保留 */
    .word Default_Handler        /* 30: 保留 */
    .word Default_Handler        /* 31: 保留 */
    .word TMR0_IRQHandler        /* 32 */
    .word TMR1_IRQHandler        /* 33 */
    .word TMR2_IRQHandler        /* 34 */
    .word TMR3_IRQHandler        /* 35 */
    .word Default_Handler        /* 36: 保留 */
    .word Default_Handler        /* 37: 保留 */
    .word Default_Handler        /* 38: 保留 */
    .word Default_Handler        /* 39: 保留 */
    .word PDMA_IRQHandler        /* 40 */
    .word Default_Handler        /* 41: 保留 */
    .word ADC_IRQHandler         /* 42 */
    .word Default_Handler        /* 43: 保留 */
    .word Default_Handler        /* 44: 保留 */
    .word Default_Handler        /* 45: 保留 */
    .word WDT_IRQHandler         /* 46 */
    .word WWDT_IRQHandler        /* 47 */
    .word EADC0_IRQHandler       /* 48 */
    .word EADC1_IRQHandler       /* 49 */
    .word EADC2_IRQHandler       /* 50 */
    .word EADC3_IRQHandler       /* 51 */
    .word Default_Handler        /* 52: 保留 */
    .word Default_Handler        /* 53: 保留 */
    .word Default_Handler        /* 54: 保留 */
    .word Default_Handler        /* 55: 保留 */
    .word ACMP_IRQHandler        /* 56 */
    .word Default_Handler        /* 57: 保留 */
    .word Default_Handler        /* 58: 保留 */
    .word Default_Handler        /* 59: 保留 */
    .word OPA0_IRQHandler        /* 60 */
    .word OPA1_IRQHandler        /* 61 */
    .word ICAP0_IRQHandler       /* 62 */
    .word ICAP1_IRQHandler       /* 63 */
    .word PWM0CH0_IRQHandler     /* 64 */
    .word PWM0CH1_IRQHandler     /* 65 */
    .word PWM0CH2_IRQHandler     /* 66 */
    .word PWM0CH3_IRQHandler     /* 67 */
    .word PWM0CH4_IRQHandler     /* 68 */
    .word PWM0CH5_IRQHandler     /* 69 */
    .word PWM0_BRK_IRQHandler    /* 70 */
    .word QEI0_IRQHandler        /* 71 */
    .word PWM1CH0_IRQHandler     /* 72 */
    .word PWM1CH1_IRQHandler     /* 73 */
    .word PWM1CH2_IRQHandler     /* 74 */
    .word PWM1CH3_IRQHandler     /* 75 */
    .word PWM1CH4_IRQHandler     /* 76 */
    .word PWM1CH5_IRQHandler     /* 77 */
    .word PWM1_BRK_IRQHandler    /* 78 */
    .word QEI1_IRQHandler        /* 79 */
    .word EPWM0_IRQHandler       /* 80 */
    .word EPWM0BRK_IRQHandler    /* 81 */
    .word EPWM1_IRQHandler       /* 82 */
    .word EPWM1BRK_IRQHandler    /* 83 */
    .word Default_Handler        /* 84: 保留 */
    .word Default_Handler        /* 85: 保留 */
    .word Default_Handler        /* 86: 保留 */
    .word Default_Handler        /* 87: 保留 */
    .word USBD_IRQHandler        /* 88 */
    .word USBH_IRQHandler        /* 89 */
    .word USB_OTG_IRQHandler     /* 90 */
    .word Default_Handler        /* 91: 保留 */
    .word EMAC_TX_IRQHandler     /* 92 */
    .word EMAC_RX_IRQHandler     /* 93 */
    .word Default_Handler        /* 94: 保留 */
    .word Default_Handler        /* 95: 保留 */
    .word SPI0_IRQHandler        /* 96 */
    .word SPI1_IRQHandler        /* 97 */
    .word SPI2_IRQHandler        /* 98 */
    .word SPI3_IRQHandler        /* 99 */
    .word Default_Handler        /* 100: 保留 */
    .word Default_Handler        /* 101: 保留 */
    .word Default_Handler        /* 102: 保留 */
    .word Default_Handler        /* 103: 保留 */
    .word UART0_IRQHandler       /* 104 */
    .word UART1_IRQHandler       /* 105 */
    .word UART2_IRQHandler       /* 106 */
    .word UART3_IRQHandler       /* 107 */
    .word UART4_IRQHandler       /* 108 */
    .word UART5_IRQHandler       /* 109 */
    .word Default_Handler        /* 110: 保留 */
    .word Default_Handler        /* 111: 保留 */
    .word I2C0_IRQHandler        /* 112 */
    .word I2C1_IRQHandler        /* 113 */
    .word I2C2_IRQHandler        /* 114 */
    .word I2C3_IRQHandler        /* 115 */
    .word I2C4_IRQHandler        /* 116 */
    .word Default_Handler        /* 117: 保留 */
    .word Default_Handler        /* 118: 保留 */
    .word Default_Handler        /* 119: 保留 */
    .word SC0_IRQHandler         /* 120 */
    .word SC1_IRQHandler         /* 121 */
    .word SC2_IRQHandler         /* 122 */
    .word SC3_IRQHandler         /* 123 */
    .word SC4_IRQHandler         /* 124 */
    .word SC5_IRQHandler         /* 125 */
    .word Default_Handler        /* 126: 保留 */
    .word Default_Handler        /* 127: 保留 */
    .word CAN0_IRQHandler        /* 128 */
    .word CAN1_IRQHandler        /* 129 */
    .word Default_Handler        /* 130: 保留 */
    .word Default_Handler        /* 131: 保留 */
    .word I2S0_IRQHandler        /* 132 */
    .word I2S1_IRQHandler        /* 133 */
    .word Default_Handler        /* 134: 保留 */
    .word Default_Handler        /* 135: 保留 */
    .word SD_IRQHandler          /* 136 */
    .word Default_Handler        /* 137: 保留 */
    .word PS2D_IRQHandler        /* 138 */
    .word CAP_IRQHandler         /* 139 */
    .word CRYPTO_IRQHandler      /* 140 */
    .word CRC_IRQHandler         /* 141 */

    .weak BOD_IRQHandler
    .thumb_set BOD_IRQHandler, Default_Handler
    .weak IRC_IRQHandler
    .thumb_set IRC_IRQHandler, Default_Handler
    .weak PWRWU_IRQHandler
    .thumb_set PWRWU_IRQHandler, Default_Handler
    .weak RAMPE_IRQHandler
    .thumb_set RAMPE_IRQHandler, Default_Handler
    .weak CKFAIL_IRQHandler
    .thumb_set CKFAIL_IRQHandler, Default_Handler
    .weak RTC_IRQHandler
    .thumb_set RTC_IRQHandler, Default_Handler
    .weak TAMPER_IRQHandler
    .thumb_set TAMPER_IRQHandler, Default_Handler
    .weak EINT0_IRQHandler
    .thumb_set EINT0_IRQHandler, Default_Handler
    .weak EINT1_IRQHandler
    .thumb_set EINT1_IRQHandler, Default_Handler
    .weak EINT2_IRQHandler
    .thumb_set EINT2_IRQHandler, Default_Handler
    .weak EINT3_IRQHandler
    .thumb_set EINT3_IRQHandler, Default_Handler
    .weak EINT4_IRQHandler
    .thumb_set EINT4_IRQHandler, Default_Handler
    .weak EINT5_IRQHandler
    .thumb_set EINT5_IRQHandler, Default_Handler
    .weak EINT6_IRQHandler
    .thumb_set EINT6_IRQHandler, Default_Handler
    .weak EINT7_IRQHandler
    .thumb_set EINT7_IRQHandler, Default_Handler
    .weak GPA_IRQHandler
    .thumb_set GPA_IRQHandler, Default_Handler
    .weak GPB_IRQHandler
    .thumb_set GPB_IRQHandler, Default_Handler
    .weak GPC_IRQHandler
    .thumb_set GPC_IRQHandler, Default_Handler
    .weak GPD_IRQHandler
    .thumb_set GPD_IRQHandler, Default_Handler
    .weak GPE_IRQHandler
    .thumb_set GPE_IRQHandler, Default_Handler
    .weak GPF_IRQHandler
    .thumb_set GPF_IRQHandler, Default_Handler
    .weak GPG_IRQHandler
    .thumb_set GPG_IRQHandler, Default_Handler
    .weak GPH_IRQHandler
    .thumb_set GPH_IRQHandler, Default_Handler
    .weak GPI_IRQHandler
    .thumb_set GPI_IRQHandler, Default_Handler
    .weak TMR0_IRQHandler
    .thumb_set TMR0_IRQHandler, Default_Handler
    .weak TMR1_IRQHandler
    .thumb_set TMR1_IRQHandler, Default_Handler
    .weak TMR2_IRQHandler
    .thumb_set TMR2_IRQHandler, Default_Handler
    .weak TMR3_IRQHandler
    .thumb_set TMR3_IRQHandler, Default_Handler
    .weak PDMA_IRQHandler
    .thumb_set PDMA_IRQHandler, Default_Handler
    .weak ADC_IRQHandler
    .thumb_set ADC_IRQHandler, Default_Handler
    .weak WDT_IRQHandler
    .thumb_set WDT_IRQHandler, Default_Handler
    .weak WWDT_IRQHandler
    .thumb_set WWDT_IRQHandler, Default_Handler
    .weak EADC0_IRQHandler
    .thumb_set EADC0_IRQHandler, Default_Handler
    .weak EADC1_IRQHandler
    .thumb_set EADC1_IRQHandler, Default_Handler
    .weak EADC2_IRQHandler
    .thumb_set EADC2_IRQHandler, Default_Handler
    .weak EADC3_IRQHandler
    .thumb_set EADC3_IRQHandler, Default_Handler
    .weak ACMP_IRQHandler
    .thumb_set ACMP_IRQHandler, Default_Handler
    .weak OPA0_IRQHandler
    .thumb_set OPA0_IRQHandler, Default_Handler
    .weak OPA1_IRQHandler
    .thumb_set OPA1_IRQHandler, Default_Handler
    .weak ICAP0_IRQHandler
    .thumb_set ICAP0_IRQHandler, Default_Handler
    .weak ICAP1_IRQHandler
    .thumb_set ICAP1_IRQHandler, Default_Handler
    .weak PWM0CH0_IRQHandler
    .thumb_set PWM0CH0_IRQHandler, Default_Handler
    .weak PWM0CH1_IRQHandler
    .thumb_set PWM0CH1_IRQHandler, Default_Handler
    .weak PWM0CH2_IRQHandler
    .thumb_set PWM0CH2_IRQHandler, Default_Handler
    .weak PWM0CH3_IRQHandler
    .thumb_set PWM0CH3_IRQHandler, Default_Handler
    .weak PWM0CH4_IRQHandler
    .thumb_set PWM0CH4_IRQHandler, Default_Handler
    .weak PWM0CH5_IRQHandler
    .thumb_set PWM0CH5_IRQHandler, Default_Handler
    .weak PWM0_BRK_IRQHandler
    .thumb_set PWM0_BRK_IRQHandler, Default_Handler
    .weak QEI0_IRQHandler
    .thumb_set QEI0_IRQHandler, Default_Handler
    .weak PWM1CH0_IRQHandler
    .thumb_set PWM1CH0_IRQHandler, Default_Handler
    .weak PWM1CH1_IRQHandler
    .thumb_set PWM1CH1_IRQHandler, Default_Handler
    .weak PWM1CH2_IRQHandler
    .thumb_set PWM1CH2_IRQHandler, Default_Handler
    .weak PWM1CH3_IRQHandler
    .thumb_set PWM1CH3_IRQHandler, Default_Handler
    .weak PWM1CH4_IRQHandler
    .thumb_set PWM1CH4_IRQHandler, Default_Handler
    .weak PWM1CH5_IRQHandler
    .thumb_set PWM1CH5_IRQHandler, Default_Handler
    .weak PWM1_BRK_IRQHandler
    .thumb_set PWM1_BRK_IRQHandler, Default_Handler
    .weak QEI1_IRQHandler
    .thumb_set QEI1_IRQHandler, Default_Handler
    .weak EPWM0_IRQHandler
    .thumb_set EPWM0_IRQHandler, Default_Handler
    .weak EPWM0BRK_IRQHandler
    .thumb_set EPWM0BRK_IRQHandler, Default_Handler
    .weak EPWM1_IRQHandler
    .thumb_set EPWM1_IRQHandler, Default_Handler
    .weak EPWM1BRK_IRQHandler
    .thumb_set EPWM1BRK_IRQHandler, Default_Handler
    .weak USBD_IRQHandler
    .thumb_set USBD_IRQHandler, Default_Handler
    .weak USBH_IRQHandler
    .thumb_set USBH_IRQHandler, Default_Handler
    .weak USB_OTG_IRQHandler
    .thumb_set USB_OTG_IRQHandler, Default_Handler
    .weak EMAC_TX_IRQHandler
    .thumb_set EMAC_TX_IRQHandler, Default_Handler
    .weak EMAC_RX_IRQHandler
    .thumb_set EMAC_RX_IRQHandler, Default_Handler
    .weak SPI0_IRQHandler
    .thumb_set SPI0_IRQHandler, Default_Handler
    .weak SPI1_IRQHandler
    .thumb_set SPI1_IRQHandler, Default_Handler
    .weak SPI2_IRQHandler
    .thumb_set SPI2_IRQHandler, Default_Handler
    .weak SPI3_IRQHandler
    .thumb_set SPI3_IRQHandler, Default_Handler
    .weak UART0_IRQHandler
    .thumb_set UART0_IRQHandler, Default_Handler
    .weak UART1_IRQHandler
    .thumb_set UART1_IRQHandler, Default_Handler
    .weak UART2_IRQHandler
    .thumb_set UART2_IRQHandler, Default_Handler
    .weak UART3_IRQHandler
    .thumb_set UART3_IRQHandler, Default_Handler
    .weak UART4_IRQHandler
    .thumb_set UART4_IRQHandler, Default_Handler
    .weak UART5_IRQHandler
    .thumb_set UART5_IRQHandler, Default_Handler
    .weak I2C0_IRQHandler
    .thumb_set I2C0_IRQHandler, Default_Handler
    .weak I2C1_IRQHandler
    .thumb_set I2C1_IRQHandler, Default_Handler
    .weak I2C2_IRQHandler
    .thumb_set I2C2_IRQHandler, Default_Handler
    .weak I2C3_IRQHandler
    .thumb_set I2C3_IRQHandler, Default_Handler
    .weak I2C4_IRQHandler
    .thumb_set I2C4_IRQHandler, Default_Handler
    .weak SC0_IRQHandler
    .thumb_set SC0_IRQHandler, Default_Handler
    .weak SC1_IRQHandler
    .thumb_set SC1_IRQHandler, Default_Handler
    .weak SC2_IRQHandler
    .thumb_set SC2_IRQHandler, Default_Handler
    .weak SC3_IRQHandler
    .thumb_set SC3_IRQHandler, Default_Handler
    .weak SC4_IRQHandler
    .thumb_set SC4_IRQHandler, Default_Handler
    .weak SC5_IRQHandler
    .thumb_set SC5_IRQHandler, Default_Handler
    .weak CAN0_IRQHandler
    .thumb_set CAN0_IRQHandler, Default_Handler
    .weak CAN1_IRQHandler
    .thumb_set CAN1_IRQHandler, Default_Handler
    .weak I2S0_IRQHandler
    .thumb_set I2S0_IRQHandler, Default_Handler
    .weak I2S1_IRQHandler
    .thumb_set I2S1_IRQHandler, Default_Handler
    .weak SD_IRQHandler
    .thumb_set SD_IRQHandler, Default_Handler
    .weak PS2D_IRQHandler
    .thumb_set PS2D_IRQHandler, Default_Handler
    .weak CAP_IRQHandler
    .thumb_set CAP_IRQHandler, Default_Handler
    .weak CRYPTO_IRQHandler
    .thumb_set CRYPTO_IRQHandler, Default_Handler
    .weak CRC_IRQHandler
    .thumb_set CRC_IRQHandler, Default_Handler
