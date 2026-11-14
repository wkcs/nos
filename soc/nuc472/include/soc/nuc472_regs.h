/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472/NUC442 片上系统寄存器结构与基地址定义 (严格使用 stdint.h)
 */
#ifndef __NOS_SOC_NUC472_REGS_H__
#define __NOS_SOC_NUC472_REGS_H__

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef __IO
#define __IO volatile
#endif
#ifndef __I
#define __I  volatile const
#endif
#ifndef __O
#define __O  volatile
#endif

/* =========================================================================
 * 存储器与总线基地址 (NUC472 TRM)
 * ========================================================================= */
#define NUC472_APROM_BASE         0x00000000UL
#define NUC472_APROM_SIZE         0x00080000UL  /* 512 KiB 片内 APROM */
#define NUC472_SRAM_BASE          0x20000000UL
#define NUC472_SRAM_SIZE          0x00010000UL  /* 64 KiB 片内 SRAM */

#define NUC472_SYS_BASE           0x40000000UL
#define NUC472_CLK_BASE           0x40000200UL
#define NUC472_INT_BASE           0x40000300UL
#define NUC472_GPIO_BASE          0x40004000UL
#define NUC472_EMAC_BASE          0x4000B000UL
#define NUC472_FMC_BASE           0x4000C000UL
#define NUC472_USBD_BASE          0x40019000UL

#define NUC472_WDT_BASE           0x40040000UL
#define NUC472_RTC_BASE           0x40041000UL
#define NUC472_EADC_BASE          0x40044000UL
#define NUC472_TIMER0_BASE        0x40050000UL
#define NUC472_TIMER1_BASE        0x40050020UL
#define NUC472_TIMER2_BASE        0x40051000UL
#define NUC472_TIMER3_BASE        0x40051020UL
#define NUC472_PWM0_BASE          0x40058000UL
#define NUC472_PWM1_BASE          0x40059000UL
#define NUC472_SPI0_BASE          0x40060000UL
#define NUC472_SPI1_BASE          0x40061000UL
#define NUC472_SPI2_BASE          0x40062000UL
#define NUC472_SPI3_BASE          0x40063000UL
#define NUC472_UART0_BASE         0x40070000UL
#define NUC472_I2C0_BASE          0x40080000UL
#define NUC472_I2C1_BASE          0x40081000UL
#define NUC472_I2C2_BASE          0x40082000UL
#define NUC472_I2C3_BASE          0x40083000UL
#define NUC472_I2C4_BASE          0x40084000UL

#define NUC472_GPIO_PORT(n)       (NUC472_GPIO_BASE + ((uint32_t)(n) * 0x40UL))
#define NUC472_GPIOA_BASE         NUC472_GPIO_PORT(0U)
#define NUC472_GPIOB_BASE         NUC472_GPIO_PORT(1U)
#define NUC472_GPIOC_BASE         NUC472_GPIO_PORT(2U)
#define NUC472_GPIOD_BASE         NUC472_GPIO_PORT(3U)
#define NUC472_GPIOE_BASE         NUC472_GPIO_PORT(4U)
#define NUC472_GPIOF_BASE         NUC472_GPIO_PORT(5U)
#define NUC472_GPIOG_BASE         NUC472_GPIO_PORT(6U)
#define NUC472_GPIOH_BASE         NUC472_GPIO_PORT(7U)
#define NUC472_GPIOI_BASE         NUC472_GPIO_PORT(8U)

/* =========================================================================
 * NUC472 外部中断号枚举 (0..141, 严格对齐 BSP startup_NUC472_442.S)
 * ========================================================================= */
typedef enum {
    NUC472_BOD_IRQn       = 0,
    NUC472_IRC_IRQn       = 1,
    NUC472_PWRWU_IRQn     = 2,
    NUC472_SRAMF_IRQn     = 3,
    NUC472_CLKF_IRQn      = 4,
    NUC472_RTC_IRQn       = 6,
    NUC472_TAMPER_IRQn    = 7,
    NUC472_EINT0_IRQn     = 8,
    NUC472_EINT1_IRQn     = 9,
    NUC472_EINT2_IRQn     = 10,
    NUC472_EINT3_IRQn     = 11,
    NUC472_EINT4_IRQn     = 12,
    NUC472_EINT5_IRQn     = 13,
    NUC472_EINT6_IRQn     = 14,
    NUC472_EINT7_IRQn     = 15,
    NUC472_GPA_IRQn       = 16,
    NUC472_GPB_IRQn       = 17,
    NUC472_GPC_IRQn       = 18,
    NUC472_GPD_IRQn       = 19,
    NUC472_GPE_IRQn       = 20,
    NUC472_GPF_IRQn       = 21,
    NUC472_GPG_IRQn       = 22,
    NUC472_GPH_IRQn       = 23,
    NUC472_GPI_IRQn       = 24,
    NUC472_TMR0_IRQn      = 32,
    NUC472_TMR1_IRQn      = 33,
    NUC472_TMR2_IRQn      = 34,
    NUC472_TMR3_IRQn      = 35,
    NUC472_PDMA_IRQn      = 40,
    NUC472_ADC_IRQn       = 42,
    NUC472_WDT_IRQn       = 46,
    NUC472_WWDT_IRQn      = 47,
    NUC472_EADC0_IRQn     = 48,
    NUC472_EADC1_IRQn     = 49,
    NUC472_EADC2_IRQn     = 50,
    NUC472_EADC3_IRQn     = 51,
    NUC472_PWM0CH0_IRQn   = 64,
    NUC472_PWM0CH1_IRQn   = 65,
    NUC472_PWM0CH2_IRQn   = 66,
    NUC472_PWM0CH3_IRQn   = 67,
    NUC472_PWM0CH4_IRQn   = 68,
    NUC472_PWM0CH5_IRQn   = 69,
    NUC472_USBD_IRQn      = 88,
    NUC472_USBH_IRQn      = 89,
    NUC472_USB_OTG_IRQn   = 90,
    NUC472_EMAC_TX_IRQn   = 92,
    NUC472_EMAC_RX_IRQn   = 93,
    NUC472_SPI0_IRQn      = 96,
    NUC472_SPI1_IRQn      = 97,
    NUC472_SPI2_IRQn      = 98,
    NUC472_SPI3_IRQn      = 99,
    NUC472_UART0_IRQn     = 104,
    NUC472_UART1_IRQn     = 105,
    NUC472_UART2_IRQn     = 106,
    NUC472_UART3_IRQn     = 107,
    NUC472_UART4_IRQn     = 108,
    NUC472_UART5_IRQn     = 109,
    NUC472_I2C0_IRQn      = 112,
    NUC472_I2C1_IRQn      = 113,
    NUC472_I2C2_IRQn      = 114,
    NUC472_I2C3_IRQn      = 115,
    NUC472_I2C4_IRQn      = 116,
    NUC472_IRQ_COUNT      = 142
} NUC472_IRQn_t;

/* =========================================================================
 * 外设寄存器结构体定义 (按 32-bit 对齐并由 _Static_assert 严格校验字节偏移)
 * ========================================================================= */
typedef struct {
    __I  uint32_t PDID;            /* 0x0000 */
    __IO uint32_t RSTSTS;          /* 0x0004 */
    __IO uint32_t IPRST0;          /* 0x0008 */
    __IO uint32_t IPRST1;          /* 0x000C */
    __IO uint32_t IPRST2;          /* 0x0010 */
    uint32_t RESERVE0[1];          /* 0x0014 */
    __IO uint32_t BODCTL;          /* 0x0018 */
    __IO uint32_t TEMPCTL;         /* 0x001C */
    __I  uint32_t VCID;            /* 0x0020 */
    __IO uint32_t PORCTL;          /* 0x0024 */
    __IO uint32_t VREFCTL;         /* 0x0028 */
    __IO uint32_t USBPHY;          /* 0x002C */
    __IO uint32_t GPA_MFPL;        /* 0x0030 */
    __IO uint32_t GPA_MFPH;        /* 0x0034 */
    __IO uint32_t GPB_MFPL;        /* 0x0038 */
    __IO uint32_t GPB_MFPH;        /* 0x003C */
    __IO uint32_t GPC_MFPL;        /* 0x0040 */
    __IO uint32_t GPC_MFPH;        /* 0x0044 */
    __IO uint32_t GPD_MFPL;        /* 0x0048 */
    __IO uint32_t GPD_MFPH;        /* 0x004C */
    __IO uint32_t GPE_MFPL;        /* 0x0050 */
    __IO uint32_t GPE_MFPH;        /* 0x0054 */
    __IO uint32_t GPF_MFPL;        /* 0x0058 */
    __IO uint32_t GPF_MFPH;        /* 0x005C */
    __IO uint32_t GPG_MFPL;        /* 0x0060 */
    __IO uint32_t GPG_MFPH;        /* 0x0064 */
    __IO uint32_t GPH_MFPL;        /* 0x0068 */
    __IO uint32_t GPH_MFPH;        /* 0x006C */
    __IO uint32_t GPI_MFPL;        /* 0x0070 */
    __IO uint32_t GPI_MFPH;        /* 0x0074 */
    uint32_t RESERVE1[18];         /* 0x0078 ~ 0x00BC */
    __IO uint32_t SRAM_INTCTL;     /* 0x00C0 */
    __IO uint32_t SRAM_STATUS;     /* 0x00C4 */
    __I  uint32_t SRAM0_ERRADDR;   /* 0x00C8 */
    __I  uint32_t SRAM1_ERRADDR;   /* 0x00CC */
    uint32_t RESERVE2[8];          /* 0x00D0 ~ 0x00EC */
    __IO uint32_t IRCTCTL;         /* 0x00F0 */
    __IO uint32_t IRCTIEN;         /* 0x00F4 */
    __IO uint32_t IRCTISTS;        /* 0x00F8 */
    uint32_t RESERVE3[1];          /* 0x00FC */
    __IO uint32_t REGLCTL;         /* 0x0100 */
} NUC472_SYS_Type;                 /* size = 0x0104 */

typedef struct {
    __IO uint32_t PWRCTL;          /* 0x0000 */
    __IO uint32_t AHBCLK;          /* 0x0004 */
    __IO uint32_t APBCLK0;         /* 0x0008 */
    __IO uint32_t APBCLK1;         /* 0x000C */
    __IO uint32_t CLKSEL0;         /* 0x0010 */
    __IO uint32_t CLKSEL1;         /* 0x0014 */
    __IO uint32_t CLKSEL2;         /* 0x0018 */
    __IO uint32_t CLKSEL3;         /* 0x001C */
    __IO uint32_t CLKDIV0;         /* 0x0020 */
    __IO uint32_t CLKDIV1;         /* 0x0024 */
    __IO uint32_t CLKDIV2;         /* 0x0028 */
    __IO uint32_t CLKDIV3;         /* 0x002C */
    uint32_t RESERVE0[4];          /* 0x0030 ~ 0x003C */
    __IO uint32_t PLLCTL;          /* 0x0040 */
    __IO uint32_t PLL2CTL;         /* 0x0044 */
    uint32_t RESERVE1[2];          /* 0x0048 ~ 0x004C */
    __IO uint32_t STATUS;          /* 0x0050 */
    uint32_t RESERVE2[3];          /* 0x0054 ~ 0x005C */
    __IO uint32_t CLKOCTL;         /* 0x0060 */
    uint32_t RESERVE3[3];          /* 0x0064 ~ 0x006C */
    __IO uint32_t CLKDCTL;         /* 0x0070 */
} NUC472_CLK_Type;                 /* size = 0x0074 */

typedef struct {
    __IO uint32_t MODE;            /* 0x0000 */
    __IO uint32_t DINOFF;          /* 0x0004 */
    __IO uint32_t DOUT;            /* 0x0008 */
    __IO uint32_t DATMSK;          /* 0x000C */
    __I  uint32_t PIN;             /* 0x0010 */
    __IO uint32_t DBEN;            /* 0x0014 */
    __IO uint32_t INTTYPE;         /* 0x0018 */
    __IO uint32_t INTEN;           /* 0x001C */
    __IO uint32_t INTSRC;          /* 0x0020 */
    __IO uint32_t SMTEN;           /* 0x0024 */
    __IO uint32_t SLEWCTL;         /* 0x0028 */
} NUC472_GPIO_Type;                /* size = 0x002C */

typedef struct {
    __IO uint32_t ISPCTL;          /* 0x0000 */
    __IO uint32_t ISPADDR;         /* 0x0004 */
    __IO uint32_t ISPDAT;          /* 0x0008 */
    __IO uint32_t ISPCMD;          /* 0x000C */
    __IO uint32_t ISPTRG;          /* 0x0010 */
    __I  uint32_t DFBA;            /* 0x0014 */
    __IO uint32_t FTCTL;           /* 0x0018 */
    uint32_t RESERVE0[9];          /* 0x001C ~ 0x003C */
    __IO uint32_t ISPSTS;          /* 0x0040 */
    __IO uint32_t FBWP;            /* 0x0044 */
    uint32_t RESERVE1[14];         /* 0x0048 ~ 0x007C */
    __IO uint32_t MPDAT0;          /* 0x0080 */
    __IO uint32_t MPDAT1;          /* 0x0084 */
    __IO uint32_t MPDAT2;          /* 0x0088 */
    __IO uint32_t MPDAT3;          /* 0x008C */
    uint32_t RESERVE2[12];         /* 0x0090 ~ 0x00BC */
    __IO uint32_t MPSTS;           /* 0x00C0 */
    __IO uint32_t MPADDR;          /* 0x00C4 */
} NUC472_FMC_Type;                 /* size = 0x00C8 */

typedef struct {
    __IO uint32_t INIT;            /* 0x0000 */
    __O  uint32_t RWEN;            /* 0x0004 */
    __IO uint32_t FREQADJ;         /* 0x0008 */
    __IO uint32_t TIME;            /* 0x000C */
    __IO uint32_t CAL;             /* 0x0010 */
    __IO uint32_t CLKFMT;          /* 0x0014 */
    __IO uint32_t WEEKDAY;         /* 0x0018 */
    __IO uint32_t TALM;            /* 0x001C */
    __IO uint32_t CALM;            /* 0x0020 */
    __I  uint32_t LEAPYEAR;        /* 0x0024 */
    __IO uint32_t INTEN;           /* 0x0028 */
    __IO uint32_t INTSTS;          /* 0x002C */
    __IO uint32_t TICK;            /* 0x0030 */
    uint32_t RESERVE0[2];          /* 0x0034 ~ 0x0038 */
    __IO uint32_t SPRCTL;          /* 0x003C */
    __IO uint32_t SPR[24];         /* 0x0040 ~ 0x009C: 后备寄存器 24 组 */
    uint32_t RESERVE1[28];         /* 0x00A0 ~ 0x010C */
    __IO uint32_t TAMPCTL;         /* 0x0110 */
    __IO uint32_t TAMPSTS;         /* 0x0114 */
    uint32_t RESERVE2[3];          /* 0x0118 ~ 0x0120 */
    __IO uint32_t TAMP0PCTL;       /* 0x0124 */
    __IO uint32_t TAMP1PCTL;       /* 0x0128 */
    __IO uint32_t LXTIPCTL;        /* 0x012C */
    __IO uint32_t LXTOPCTL;        /* 0x0130 */
    uint32_t RESERVE3[3];          /* 0x0134 ~ 0x013C */
    __IO uint32_t TAMSK;           /* 0x0140 */
    __IO uint32_t CAMSK;           /* 0x0144 */
} NUC472_RTC_Type;                 /* size = 0x0148 */

typedef struct {
    __IO uint32_t CTL;             /* 0x0000 */
    __IO uint32_t ALTCTL;          /* 0x0004 */
} NUC472_WDT_Type;                 /* size = 0x0008 */

typedef struct {
    __IO uint32_t DAT;             /* 0x0000 */
    __IO uint32_t INTEN;           /* 0x0004 */
    __IO uint32_t FIFO;            /* 0x0008 */
    __IO uint32_t LINE;            /* 0x000C */
    __IO uint32_t MODEM;           /* 0x0010 */
    __IO uint32_t MODEMSTS;        /* 0x0014 */
    __IO uint32_t FIFOSTS;         /* 0x0018 */
    __IO uint32_t INTSTS;          /* 0x001C */
    __IO uint32_t TOUT;            /* 0x0020 */
    __IO uint32_t BAUD;            /* 0x0024 */
    __IO uint32_t IRDA;            /* 0x0028 */
    __IO uint32_t ALTCTL;          /* 0x002C */
    __IO uint32_t FUNCSEL;         /* 0x0030 */
    __IO uint32_t LINCTL;          /* 0x0034 */
    __IO uint32_t LINSTS;          /* 0x0038 */
    __IO uint32_t LINDEBUG;        /* 0x003C */
} NUC472_UART_Type;                /* size = 0x0040 */

typedef struct {
    __IO uint32_t CTL;             /* 0x0000 */
    __IO uint32_t CLKDIV;          /* 0x0004 */
    __IO uint32_t SSCTL;           /* 0x0008 */
    __IO uint32_t PDMACTL;         /* 0x000C */
    __IO uint32_t FIFOCTL;         /* 0x0010 */
    __IO uint32_t STATUS;          /* 0x0014 */
    uint32_t RESERVE0[2];          /* 0x0018 ~ 0x001C */
    __O  uint32_t TX;              /* 0x0020 */
    uint32_t RESERVE1[3];          /* 0x0024 ~ 0x002C */
    __I  uint32_t RX;              /* 0x0030 */
} NUC472_SPI_Type;                 /* size = 0x0034 */

typedef struct {
    __IO uint32_t CTL;             /* 0x0000 */
    __IO uint32_t ADDR0;           /* 0x0004 */
    __IO uint32_t DAT;             /* 0x0008 */
    __I  uint32_t STATUS;          /* 0x000C */
    __IO uint32_t CLKDIV;          /* 0x0010 */
    __IO uint32_t TOCTL;           /* 0x0014 */
    __IO uint32_t ADDR1;           /* 0x0018 */
    __IO uint32_t ADDR2;           /* 0x001C */
    __IO uint32_t ADDR3;           /* 0x0020 */
    __IO uint32_t ADDRMSK0;        /* 0x0024 */
    __IO uint32_t ADDRMSK1;        /* 0x0028 */
    __IO uint32_t ADDRMSK2;        /* 0x002C */
    __IO uint32_t ADDRMSK3;        /* 0x0030 */
    uint32_t RESERVE0[2];          /* 0x0034 ~ 0x0038 */
    __IO uint32_t WKCTL;           /* 0x003C */
    __IO uint32_t WKSTS;           /* 0x0040 */
} NUC472_I2C_Type;                 /* size = 0x0044 */

typedef struct {
    __IO uint32_t CTL;             /* 0x0000 */
    __IO uint32_t CMP;             /* 0x0004 */
    __IO uint32_t INTSTS;          /* 0x0008 */
    __I  uint32_t CNT;             /* 0x000C */
    __I  uint32_t CAP;             /* 0x0010 */
    __IO uint32_t EXTCTL;          /* 0x0014 */
    __IO uint32_t EINTSTS;         /* 0x0018 */
} NUC472_TIMER_Type;               /* size = 0x001C */

typedef struct {
    __IO uint32_t CLKPSC;          /* 0x0000 */
    __IO uint32_t CLKDIV;          /* 0x0004 */
    __IO uint32_t CTL;             /* 0x0008 */
    __IO uint32_t CNTEN;           /* 0x000C */
    __IO uint32_t PERIOD[6];       /* 0x0010 ~ 0x0024 */
    __IO uint32_t CMPDAT[6];       /* 0x0028 ~ 0x003C */
    __I  uint32_t CNT[6];          /* 0x0040 ~ 0x0054 */
    __IO uint32_t MSKEN;           /* 0x0058 */
    __IO uint32_t MSK;             /* 0x005C */
    __IO uint32_t DTCTL;           /* 0x0060 */
    __IO uint32_t TRGADCTL;        /* 0x0064 */
    __IO uint32_t TRGADCSTS;       /* 0x0068 */
    __IO uint32_t BRKCTL;          /* 0x006C */
    __IO uint32_t INTCTL;          /* 0x0070 */
    __IO uint32_t INTEN;           /* 0x0074 */
    __IO uint32_t INTSTS;          /* 0x0078 */
    __IO uint32_t POEN;            /* 0x007C */
    __IO uint32_t CAPCTL;          /* 0x0080 */
    __IO uint32_t CAPINEN;         /* 0x0084 */
    __I  uint32_t CAPSTS;          /* 0x0088 */
    uint32_t RESERVE0[1];          /* 0x008C */
    __I  uint32_t RCAPDAT0;        /* 0x0090 */
    __I  uint32_t FCAPDAT0;        /* 0x0094 */
    __I  uint32_t RCAPDAT1;        /* 0x0098 */
    __I  uint32_t FCAPDAT1;        /* 0x009C */
    __I  uint32_t RCAPDAT2;        /* 0x00A0 */
    __I  uint32_t FCAPDAT2;        /* 0x00A4 */
    __I  uint32_t RCAPDAT3;        /* 0x00A8 */
    __I  uint32_t FCAPDAT3;        /* 0x00AC */
    __I  uint32_t RCAPDAT4;        /* 0x00B0 */
    __I  uint32_t FCAPDAT4;        /* 0x00B4 */
    __I  uint32_t RCAPDAT5;        /* 0x00B8 */
    __I  uint32_t FCAPDAT5;        /* 0x00BC */
    uint32_t RESERVE1[8];          /* 0x00C0 ~ 0x00DC */
    __I  uint32_t SBS[6];          /* 0x00E0 ~ 0x00F4 */
} NUC472_PWM_Type;                 /* size = 0x00F8 */

typedef struct {
    __I  uint32_t AD0DAT0;         /* 0x0000 */
    __I  uint32_t AD0DAT1;         /* 0x0004 */
    __I  uint32_t AD0DAT2;         /* 0x0008 */
    __I  uint32_t AD0DAT3;         /* 0x000C */
    __I  uint32_t AD0DAT4;         /* 0x0010 */
    __I  uint32_t AD0DAT5;         /* 0x0014 */
    __I  uint32_t AD0DAT6;         /* 0x0018 */
    __I  uint32_t AD0DAT7;         /* 0x001C */
    __I  uint32_t AD1DAT0;         /* 0x0020 */
    __I  uint32_t AD1DAT1;         /* 0x0024 */
    __I  uint32_t AD1DAT2;         /* 0x0028 */
    __I  uint32_t AD1DAT3;         /* 0x002C */
    __I  uint32_t AD1DAT4;         /* 0x0030 */
    __I  uint32_t AD1DAT5;         /* 0x0034 */
    __I  uint32_t AD1DAT6;         /* 0x0038 */
    __I  uint32_t AD1DAT7;         /* 0x003C */
    __IO uint32_t CTL;             /* 0x0040 */
    uint32_t RESERVE0[1];          /* 0x0044 */
    __O  uint32_t SWTRG;           /* 0x0048 */
    __I  uint32_t PENDSTS;         /* 0x004C */
    __IO uint32_t ADIFOV;          /* 0x0050 */
    __IO uint32_t OVSTS;           /* 0x0054 */
    __IO uint32_t AD0SPCTL0;       /* 0x0058 */
    __IO uint32_t AD0SPCTL1;       /* 0x005C */
    __IO uint32_t AD0SPCTL2;       /* 0x0060 */
    __IO uint32_t AD0SPCTL3;       /* 0x0064 */
    __IO uint32_t AD0SPCTL4;       /* 0x0068 */
    __IO uint32_t AD0SPCTL5;       /* 0x006C */
    __IO uint32_t AD0SPCTL6;       /* 0x0070 */
    __IO uint32_t AD0SPCTL7;       /* 0x0074 */
    __IO uint32_t AD1SPCTL0;       /* 0x0078 */
    __IO uint32_t AD1SPCTL1;       /* 0x007C */
    __IO uint32_t AD1SPCTL2;       /* 0x0080 */
    __IO uint32_t AD1SPCTL3;       /* 0x0084 */
    __IO uint32_t AD1SPCTL4;       /* 0x0088 */
    __IO uint32_t AD1SPCTL5;       /* 0x008C */
    __IO uint32_t AD1SPCTL6;       /* 0x0090 */
    __IO uint32_t AD1SPCTL7;       /* 0x0094 */
    uint32_t RESERVE1[3];          /* 0x0098 ~ 0x00A0 */
    __IO uint32_t SIMUSEL;         /* 0x00A4 */
    __IO uint32_t CMP[2];          /* 0x00A8 ~ 0x00AC */
    __I  uint32_t STATUS0;         /* 0x00B0 */
    __IO uint32_t STATUS1;         /* 0x00B4 */
    __IO uint32_t EXTSMPT;         /* 0x00B8 */
    uint32_t RESERVE2[17];         /* 0x00BC ~ 0x00FC */
    __I  uint32_t AD0DDAT0;        /* 0x0100 */
    __I  uint32_t AD0DDAT1;        /* 0x0104 */
    __I  uint32_t AD0DDAT2;        /* 0x0108 */
    __I  uint32_t AD0DDAT3;        /* 0x010C */
    uint32_t RESERVE3[4];          /* 0x0110 ~ 0x011C */
    __I  uint32_t AD1DDAT0;        /* 0x0120 */
    __I  uint32_t AD1DDAT1;        /* 0x0124 */
    __I  uint32_t AD1DDAT2;        /* 0x0128 */
    __I  uint32_t AD1DDAT3;        /* 0x012C */
    __IO uint32_t DBMEN;           /* 0x0130 */
    __IO uint32_t INTSRC[4];       /* 0x0134 ~ 0x0140 */
    __IO uint32_t AD0TRGEN0;       /* 0x0144 */
    __IO uint32_t AD0TRGEN1;       /* 0x0148 */
    __IO uint32_t AD0TRGEN2;       /* 0x014C */
    __IO uint32_t AD0TRGEN3;       /* 0x0150 */
    __IO uint32_t AD1TRGEN0;       /* 0x0154 */
    __IO uint32_t AD1TRGEN1;       /* 0x0158 */
    __IO uint32_t AD1TRGEN2;       /* 0x015C */
    __IO uint32_t AD1TRGEN3;       /* 0x0160 */
} NUC472_EADC_Type;                /* size = 0x0164 */

typedef struct {
    union {
        __IO uint32_t EPDAT;       /* 0x0000 */
        __IO uint8_t  EPDAT_BYTE;  /* 0x0000 */
    };
    __IO uint32_t EPINTSTS;        /* 0x0004 */
    __IO uint32_t EPINTEN;         /* 0x0008 */
    __I  uint32_t EPDATCNT;        /* 0x000C */
    __IO uint32_t EPRSPCTL;        /* 0x0010 */
    __IO uint32_t EPMPS;           /* 0x0014 */
    __IO uint32_t EPTXCNT;         /* 0x0018 */
    __IO uint32_t EPCFG;           /* 0x001C */
    __IO uint32_t EPBUFSTART;      /* 0x0020 */
    __IO uint32_t EPBUFEND;        /* 0x0024 */
} NUC472_USBD_EP_Type;             /* size = 0x0028 */

typedef struct {
    __I  uint32_t GINTSTS;         /* 0x0000 */
    uint32_t RESERVE0[1];          /* 0x0004 */
    __IO uint32_t GINTEN;          /* 0x0008 */
    uint32_t RESERVE1[1];          /* 0x000C */
    __IO uint32_t BUSINTSTS;       /* 0x0010 */
    __IO uint32_t BUSINTEN;        /* 0x0014 */
    __IO uint32_t OPER;            /* 0x0018 */
    __I  uint32_t FRAMECNT;        /* 0x001C */
    __IO uint32_t FADDR;           /* 0x0020 */
    __IO uint32_t TEST;            /* 0x0024 */
    union {
        __IO uint32_t CEPDAT;      /* 0x0028 */
        __IO uint8_t  CEPDAT_BYTE; /* 0x0028 */
    };
    __IO uint32_t CEPCTL;          /* 0x002C */
    __IO uint32_t CEPINTEN;        /* 0x0030 */
    __IO uint32_t CEPINTSTS;       /* 0x0034 */
    __IO uint32_t CEPTXCNT;        /* 0x0038 */
    __I  uint32_t CEPRXCNT;        /* 0x003C */
    __I  uint32_t CEPDATCNT;       /* 0x0040 */
    __I  uint32_t SETUP1_0;        /* 0x0044 */
    __I  uint32_t SETUP3_2;        /* 0x0048 */
    __I  uint32_t SETUP5_4;        /* 0x004C */
    __I  uint32_t SETUP7_6;        /* 0x0050 */
    __IO uint32_t CEPBUFSTART;     /* 0x0054 */
    __IO uint32_t CEPBUFEND;       /* 0x0058 */
    __IO uint32_t DMACTL;          /* 0x005C */
    __IO uint32_t DMACNT;          /* 0x0060 */
    NUC472_USBD_EP_Type EP[12];    /* 0x0064 ~ 0x0240 */
    uint32_t RESERVE2[303];        /* 0x0244 ~ 0x06FC */
    __IO uint32_t DMAADDR;         /* 0x0700 */
    __IO uint32_t PHYCTL;          /* 0x0704 */
} NUC472_USBD_Type;                /* size = 0x0708 */

typedef struct {
    __IO uint32_t CAMCTL;          /* 0x0000 */
    __IO uint32_t CAMEN;           /* 0x0004 */
    __IO uint32_t CAM0M;           /* 0x0008 */
    __IO uint32_t CAM0L;           /* 0x000C */
    __IO uint32_t CAM1M;           /* 0x0010 */
    __IO uint32_t CAM1L;           /* 0x0014 */
    __IO uint32_t CAM2M;           /* 0x0018 */
    __IO uint32_t CAM2L;           /* 0x001C */
    __IO uint32_t CAM3M;           /* 0x0020 */
    __IO uint32_t CAM3L;           /* 0x0024 */
    __IO uint32_t CAM4M;           /* 0x0028 */
    __IO uint32_t CAM4L;           /* 0x002C */
    __IO uint32_t CAM5M;           /* 0x0030 */
    __IO uint32_t CAM5L;           /* 0x0034 */
    __IO uint32_t CAM6M;           /* 0x0038 */
    __IO uint32_t CAM6L;           /* 0x003C */
    __IO uint32_t CAM7M;           /* 0x0040 */
    __IO uint32_t CAM7L;           /* 0x0044 */
    __IO uint32_t CAM8M;           /* 0x0048 */
    __IO uint32_t CAM8L;           /* 0x004C */
    __IO uint32_t CAM9M;           /* 0x0050 */
    __IO uint32_t CAM9L;           /* 0x0054 */
    __IO uint32_t CAM10M;          /* 0x0058 */
    __IO uint32_t CAM10L;          /* 0x005C */
    __IO uint32_t CAM11M;          /* 0x0060 */
    __IO uint32_t CAM11L;          /* 0x0064 */
    __IO uint32_t CAM12M;          /* 0x0068 */
    __IO uint32_t CAM12L;          /* 0x006C */
    __IO uint32_t CAM13M;          /* 0x0070 */
    __IO uint32_t CAM13L;          /* 0x0074 */
    __IO uint32_t CAM14M;          /* 0x0078 */
    __IO uint32_t CAM14L;          /* 0x007C */
    __IO uint32_t CAM15MSB;        /* 0x0080 */
    __IO uint32_t CAM15LSB;        /* 0x0084 */
    __IO uint32_t TXDSA;           /* 0x0088 */
    __IO uint32_t RXDSA;           /* 0x008C */
    __IO uint32_t CTL;             /* 0x0090 */
    __IO uint32_t MIIMDAT;         /* 0x0094 */
    __IO uint32_t MIIMCTL;         /* 0x0098 */
    __IO uint32_t FIFOCTL;         /* 0x009C */
    __O  uint32_t TXST;            /* 0x00A0 */
    __O  uint32_t RXST;            /* 0x00A4 */
    __IO uint32_t MRFL;            /* 0x00A8 */
    __IO uint32_t INTEN;           /* 0x00AC */
    __IO uint32_t INTSTS;          /* 0x00B0 */
    __IO uint32_t GENSTS;          /* 0x00B4 */
    __IO uint32_t MPCNT;           /* 0x00B8 */
    __I  uint32_t RPCNT;           /* 0x00BC */
    uint32_t RESERVE0[2];          /* 0x00C0 ~ 0x00C4 */
    __IO uint32_t FRSTS;           /* 0x00C8 */
    __I  uint32_t CTXDSA;          /* 0x00CC */
    __I  uint32_t CTXBSA;          /* 0x00D0 */
    __I  uint32_t CRXDSA;          /* 0x00D4 */
    __I  uint32_t CRXBSA;          /* 0x00D8 */
    uint32_t RESERVE1[9];          /* 0x00DC ~ 0x00FC */
    __IO uint32_t TSCTL;           /* 0x0100 */
    uint32_t RESERVE2[3];          /* 0x0104 ~ 0x010C */
    __I  uint32_t TSSEC;           /* 0x0110 */
    __I  uint32_t TSSUBSEC;        /* 0x0114 */
    __IO uint32_t TSINC;           /* 0x0118 */
    __IO uint32_t TSADDEND;        /* 0x011C */
    __IO uint32_t UPDSEC;          /* 0x0120 */
    __IO uint32_t UPDSUBSEC;       /* 0x0124 */
    __IO uint32_t ALMSEC;          /* 0x0128 */
    __IO uint32_t ALMSUBSEC;       /* 0x012C */
} NUC472_EMAC_Type;                /* size = 0x0130 */

#ifdef __cplusplus
}
#endif

#endif /* __NOS_SOC_NUC472_REGS_H__ */
