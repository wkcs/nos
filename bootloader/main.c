/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Ultra-Compact Standalone Bootloader (STM32F401CCU6).
 *
 * Features:
 * - Confined to STM32F401 Sector 0 (0x08000000 - 0x08003FFF, < 4 KB actual size),
 *   giving NOS the full remaining 240 KB (Sectors 1..5, 0x08004000 - 0x0803FFFF).
 * - Dual-slot A/B Kernel OTA upgrade & automatic rollback from external W25QXX.
 * - Bootloader self-upgrade from W25QXX via SRAM-resident `.ramfunc`.
 * - Power-loss-safe dual-copy Boot Control Block (0x006000 & 0x007000) + RTC_BKP_DR0.
 * - Physical recovery key combos (F+SPACE -> Slot A, J+SPACE -> Slot B) & WS2812 RGB status.
 */

#include <boot_proto.h>

/* =========================================================================
 * STM32F401 Register Definitions (Zero External Library Dependencies)
 * ========================================================================= */
#define REG32(addr)                 (*(volatile uint32_t *)(uintptr_t)(addr))
#define REG16(addr)                 (*(volatile uint16_t *)(uintptr_t)(addr))
#define REG8(addr)                  (*(volatile uint8_t *)(uintptr_t)(addr))

/* Cortex-M4 SCB & NVIC */
#define SCB_VTOR                    REG32(0xE000ED08UL)
#define SCB_AIRCR                   REG32(0xE000ED0CUL)
#define SCB_CPACR                   REG32(0xE000ED88UL)
#define SYSTICK_CSR                 REG32(0xE000E010UL)
#define NVIC_ICER_BASE              0xE000E180UL
#define NVIC_ICPR_BASE              0xE000E280UL

/* RCC (0x40023800) */
#define RCC_BASE                    0x40023800UL
#define RCC_CR                      REG32(RCC_BASE + 0x00UL)
#define RCC_PLLCFGR                 REG32(RCC_BASE + 0x04UL)
#define RCC_CFGR                    REG32(RCC_BASE + 0x08UL)
#define RCC_CIR                     REG32(RCC_BASE + 0x0CUL)
#define RCC_AHB1ENR                 REG32(RCC_BASE + 0x30UL)
#define RCC_APB1RSTR                REG32(RCC_BASE + 0x20UL)
#define RCC_APB2RSTR                REG32(RCC_BASE + 0x24UL)
#define RCC_APB1ENR                 REG32(RCC_BASE + 0x40UL)
#define RCC_APB2ENR                 REG32(RCC_BASE + 0x44UL)

/* FLASH (0x40023C00) */
#define FLASH_BASE_REG              0x40023C00UL
#define FLASH_ACR                   REG32(FLASH_BASE_REG + 0x00UL)
#define FLASH_KEYR                  REG32(FLASH_BASE_REG + 0x04UL)
#define FLASH_SR                    REG32(FLASH_BASE_REG + 0x0CUL)
#define FLASH_CR                    REG32(FLASH_BASE_REG + 0x10UL)

#define FLASH_KEY1                  0x45670123UL
#define FLASH_KEY2                  0xCDEF89ABUL
#define FLASH_SR_EOP_BIT            (1UL << 0U)
#define FLASH_SR_ERR_MASK           0x000000F2UL
#define FLASH_SR_BSY_BIT            (1UL << 16U)
#define FLASH_CR_PG_BIT             (1UL << 0U)
#define FLASH_CR_SER_BIT            (1UL << 1U)
#define FLASH_CR_PSIZE_WORD         (2UL << 8U)
#define FLASH_CR_STRT_BIT           (1UL << 16U)
#define FLASH_CR_LOCK_BIT           (1UL << 31U)

/* PWR (0x40007000) & RTC (0x40002800) */
#define PWR_CR                      REG32(0x40007000UL)
#define PWR_CSR                     REG32(0x40007004UL)
#define RTC_BKP0R                   REG32(0x40002850UL)

/* GPIOA (0x40020000), GPIOB (0x40020400), GPIOC (0x40020800) */
struct stm32_gpio {
    volatile uint32_t MODER;
    volatile uint32_t OTYPER;
    volatile uint32_t OSPEEDR;
    volatile uint32_t PUPDR;
    volatile uint32_t IDR;
    volatile uint32_t ODR;
    volatile uint32_t BSRR;
    volatile uint32_t LCKR;
    volatile uint32_t AFRL;
    volatile uint32_t AFRH;
};

#define GPIOA                       ((struct stm32_gpio *)0x40020000UL)
#define GPIOB                       ((struct stm32_gpio *)0x40020400UL)
#define GPIOC                       ((struct stm32_gpio *)0x40020800UL)

/* USART1 (0x40011000) */
struct stm32_usart {
    volatile uint32_t SR;
    volatile uint32_t DR;
    volatile uint32_t BRR;
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t CR3;
    volatile uint32_t GTPR;
};
#define USART1                      ((struct stm32_usart *)0x40011000UL)

/* SPI1 (0x40013000) & SPI2 (0x40003800) */
struct stm32_spi_regs {
    volatile uint32_t CR1;
    volatile uint32_t CR2;
    volatile uint32_t SR;
    volatile uint32_t DR;
    volatile uint32_t CRCPR;
    volatile uint32_t RXCRCR;
    volatile uint32_t TXCRCR;
    volatile uint32_t I2SCFGR;
    volatile uint32_t I2SPR;
};
#define SPI1                        ((struct stm32_spi_regs *)0x40013000UL)
#define SPI2                        ((struct stm32_spi_regs *)0x40003800UL)

/* W25QXX Commands */
#define W25Q_CMD_WRITE_ENABLE       0x06U
#define W25Q_CMD_READ_STATUS1       0x05U
#define W25Q_CMD_READ_DATA          0x03U
#define W25Q_CMD_PAGE_PROGRAM       0x02U
#define W25Q_CMD_SECTOR_ERASE_4K    0x20U
#define W25Q_CMD_JEDEC_ID           0x90U

/* NK60-V2 74HC595 Raw Scan Bit Indices (from board/nk60-v2/nk60_keyboard.c) */
#define NK60_RAW_BITS               72U
#define NK60_RAW_BIT_SPACE          51U /* g_nk60_position_index[51] == 56 (SPC) */
#define NK60_RAW_BIT_F              68U /* g_nk60_position_index[68] == 32 (F) */
#define NK60_RAW_BIT_J              71U /* g_nk60_position_index[71] == 35 (J) */

#define BL_VERSION_CODE \
    (((uint32_t)(BL_VER_MAJOR) << 16U) | ((uint32_t)(BL_VER_MINOR) << 8U) | (uint32_t)(BL_VER_PATCH))

/* Static SRAM buffers (zero heap allocation) */
static uint8_t g_bl_ram_buf[NK60_BOOTLOADER_MAX_BIN];
static uint8_t g_io_buf[256];

static void delay_cycles(volatile uint32_t cycles)
{
    while (cycles-- > 0UL) {
        __asm__ volatile ("nop");
    }
}

static void delay_us(uint32_t us)
{
    delay_cycles(us * 21UL);
}

/* =========================================================================
 * Compact Nibble-Table IEEE 802.3 CRC32 (Only 64 Bytes of Flash Table!)
 * ========================================================================= */
static uint32_t bl_crc32(uint32_t init_crc, const void *data, size_t len)
{
    static const uint32_t crc_nibble[16] = {
        0x00000000UL, 0x1DB71064UL, 0x3B6E20C8UL, 0x26D930ACUL,
        0x76DC4190UL, 0x6B6B51F4UL, 0x4DB26158UL, 0x5005713CUL,
        0xEDB88320UL, 0xF00F9344UL, 0xD6D6A3E8UL, 0xCB61B38CUL,
        0x9B64C2B0UL, 0x86D3D2D4UL, 0xA00AE278UL, 0xBDBDF21CUL,
    };
    const uint8_t *p = (const uint8_t *)data;
    uint32_t crc = ~init_crc;
    size_t i;

    for (i = 0U; i < len; i++) {
        crc = (crc >> 4U) ^ crc_nibble[(crc ^ p[i]) & 0x0FU];
        crc = (crc >> 4U) ^ crc_nibble[(crc ^ (p[i] >> 4U)) & 0x0FU];
    }
    return ~crc;
}

static void bl_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- > 0U) {
        *d++ = *s++;
    }
}

/* =========================================================================
 * Clock, Backup Register & UART1 Console (PA9 TX / PA10 RX, 115200 8N1)
 * ========================================================================= */
static void clock_init_84mhz(void)
{
    uint32_t timeout = 0x5000UL;

    /* Enable FPU CP10/CP11 full access */
    SCB_CPACR |= (3UL << 20U) | (3UL << 22U);

    /* Reset RCC to HSI default */
    RCC_CR |= (1UL << 0U);
    RCC_CFGR = 0x00000000UL;
    RCC_CR &= ~((1UL << 16U) | (1UL << 18U) | (1UL << 24U));
    RCC_CIR = 0x00000000UL;

    /* Enable PWR clock & Voltage Scaling 2 */
    RCC_APB1ENR |= (1UL << 28U);
    PWR_CR |= (2UL << 14U) | (1UL << 8U); /* VOS=Scale 2, DBP=1 (unlock RTC_BKP) */

    /* Enable 8 MHz HSE */
    RCC_CR |= (1UL << 16U);
    while (((RCC_CR & (1UL << 17U)) == 0UL) && (--timeout > 0UL)) {
    }

    /* Flash 2 wait states + Prefetch + Instruction/Data Cache */
    FLASH_ACR = (2UL << 0U) | (1UL << 8U) | (1UL << 9U) | (1UL << 10U);

    /* HCLK = SYSCLK, PCLK2 = HCLK/1 (84 MHz), PCLK1 = HCLK/2 (42 MHz) */
    RCC_CFGR |= (4UL << 10U);

    if (timeout > 0UL) {
        /* PLL from 8 MHz HSE: M=8, N=336, P=4 (bits 17:16 = 01), Q=7 */
        RCC_PLLCFGR = 8UL | (336UL << 6U) | (1UL << 16U) | (1UL << 22U) | (7UL << 24U);
    } else {
        /* Fallback to 16 MHz HSI: M=16, N=336, P=4, Q=7 */
        RCC_PLLCFGR = 16UL | (336UL << 6U) | (1UL << 16U) | (7UL << 24U);
    }

    RCC_CR |= (1UL << 24U);
    while ((RCC_CR & (1UL << 25U)) == 0UL) {
    }

    RCC_CFGR = (RCC_CFGR & ~3UL) | 2UL;
    while ((RCC_CFGR & (3UL << 2U)) != (2UL << 2U)) {
    }
}

static void uart1_init(void)
{
    RCC_AHB1ENR |= (1UL << 0U);  /* GPIOA */
    RCC_APB2ENR |= (1UL << 4U);  /* USART1 */

    /* PA9 (TX) & PA10 (RX) -> AF7 */
    GPIOA->MODER = (GPIOA->MODER & ~((3UL << 18U) | (3UL << 20U))) |
                   (2UL << 18U) | (2UL << 20U);
    GPIOA->OSPEEDR |= (3UL << 18U) | (3UL << 20U);
    GPIOA->PUPDR = (GPIOA->PUPDR & ~((3UL << 18U) | (3UL << 20U))) |
                   (1UL << 18U) | (1UL << 20U);
    GPIOA->AFRH = (GPIOA->AFRH & ~((0xFUL << 4U) | (0xFUL << 8U))) |
                  (7UL << 4U) | (7UL << 8U);

    /* 84 MHz / 115200 = 729 (0x02D9) */
    USART1->BRR = 0x02D9UL;
    USART1->CR1 = (1UL << 13U) | (1UL << 3U); /* UE | TE */
}

static void uart_putc(char c)
{
    while ((USART1->SR & (1UL << 7U)) == 0UL) {
    }
    USART1->DR = (uint8_t)c;
}

static void uart_puts(const char *s)
{
    while (*s != '\0') {
        uart_putc(*s++);
    }
}

static void uart_put_hex(uint32_t val)
{
    int shift;
    uart_puts("0x");
    for (shift = 28; shift >= 0; shift -= 4) {
        uint32_t nib = (val >> (uint32_t)shift) & 0x0FUL;
        uart_putc((char)(nib < 10UL ? (0x30UL + nib) : (0x61UL + nib - 10UL)));
    }
}

/* =========================================================================
 * WS2812 RGB Status Indicator (PB0 LED_VCC_EN, PC3 SPI2_MOSI)
 * ========================================================================= */
static void rgb_init(void)
{
    RCC_AHB1ENR |= (1UL << 1U) | (1UL << 2U); /* GPIOB, GPIOC */
    RCC_APB1ENR |= (1UL << 14U);              /* SPI2 */

    /* PB0: LED_VCC_EN push-pull output, initially low (off) */
    GPIOB->MODER = (GPIOB->MODER & ~(3UL << 0U)) | (1UL << 0U);
    GPIOB->BSRR = (1UL << 16U);

    /* PC3: SPI2_MOSI AF5 */
    GPIOC->MODER = (GPIOC->MODER & ~(3UL << 6U)) | (2UL << 6U);
    GPIOC->OSPEEDR |= (3UL << 6U);
    GPIOC->AFRL = (GPIOC->AFRL & ~(0xFUL << 12U)) | (5UL << 12U);

    /* SPI2 Master, 1-line TX, CPOL=1, CPHA=1, Soft NSS, Prescaler=8 (42MHz/8 = 5.25MHz) */
    SPI2->CR1 = (1UL << 15U) | (1UL << 14U) | (1UL << 9U) | (1UL << 8U) |
                (2UL << 3U) | (1UL << 2U) | (1UL << 1U) | (1UL << 0U) | (1UL << 6U);
}

static void rgb_spi_send_byte(uint8_t val)
{
    int i;
    for (i = 0; i < 8; i++) {
        uint8_t wire = ((val << i) & 0x80U) ? 0xF0U : 0xC0U;
        while ((SPI2->SR & (1UL << 1U)) == 0UL) {
        }
        SPI2->DR = wire;
    }
}

static void rgb_set_solid(uint8_t r, uint8_t g, uint8_t b)
{
    int i;
    GPIOB->BSRR = (1UL << 0U); /* Enable LED power (PB0 = 1) */
    delay_us(200U);
    for (i = 0; i < 61; i++) {
        rgb_spi_send_byte(g);
        rgb_spi_send_byte(r);
        rgb_spi_send_byte(b);
    }
    while ((SPI2->SR & (1UL << 7U)) != 0UL) {
    }
    delay_us(100U);
}

/* =========================================================================
 * 74HC595 Key Matrix Recovery Scan (PB12 EN, PB13 CLK, PB15 DATA, PB14 IN)
 * ========================================================================= */
static uint8_t key_scan_recovery_cmd(void)
{
    uint32_t i;
    bool f_down = false;
    bool j_down = false;
    bool spc_down = false;

    RCC_AHB1ENR |= (1UL << 1U); /* GPIOB */

    /* PB12, PB13, PB15 outputs; PB14 input with pull-up */
    GPIOB->MODER = (GPIOB->MODER & ~((3UL << 24U) | (3UL << 26U) | (3UL << 28U) | (3UL << 30U))) |
                   (1UL << 24U) | (1UL << 26U) | (1UL << 30U);
    GPIOB->PUPDR = (GPIOB->PUPDR & ~(3UL << 28U)) | (1UL << 28U);
    GPIOB->BSRR = (1UL << (12U + 16U)) | (1UL << (13U + 16U)) | (1UL << (15U + 16U));
    delay_us(10U);

    for (i = 0UL; i < NK60_RAW_BITS; i++) {
        /* Shift active-low 0 at bit 0, then 1s across all 72 bits */
        if (i == 0UL) {
            GPIOB->BSRR = (1UL << (15U + 16U));
        } else {
            GPIOB->BSRR = (1UL << 15U);
        }
        delay_us(1U);
        GPIOB->BSRR = (1UL << 13U);
        delay_us(1U);
        GPIOB->BSRR = (1UL << (13U + 16U));
        GPIOB->BSRR = (1UL << 12U);
        delay_us(1U);
        GPIOB->BSRR = (1UL << (12U + 16U));
        delay_us(10U);

        if ((GPIOB->IDR & (1UL << 14U)) == 0UL) {
            if (i == NK60_RAW_BIT_F) {
                f_down = true;
            } else if (i == NK60_RAW_BIT_J) {
                j_down = true;
            } else if (i == NK60_RAW_BIT_SPACE) {
                spc_down = true;
            }
        }
    }

    if (spc_down && (f_down || j_down)) {
        return NK60_BOOT_CMD_UPDATE_KERNEL;
    }
    return NK60_BOOT_CMD_NONE;
}

/* =========================================================================
 * W25QXX SPI1 Driver (PB2 CS, PB3 SCK, PB4 MISO, PB5 MOSI)
 * ========================================================================= */
static inline void w25q_cs_low(void)
{
    GPIOB->BSRR = (1UL << (2U + 16U));
}

static inline void w25q_cs_high(void)
{
    GPIOB->BSRR = (1UL << 2U);
}

static uint8_t w25q_xfer(uint8_t tx)
{
    while ((SPI1->SR & (1UL << 1U)) == 0UL) {
    }
    SPI1->DR = tx;
    while ((SPI1->SR & (1UL << 0U)) == 0UL) {
    }
    return (uint8_t)SPI1->DR;
}

static void w25q_init(void)
{
    RCC_AHB1ENR |= (1UL << 1U); /* GPIOB */
    RCC_APB2ENR |= (1UL << 12U); /* SPI1 */

    /* PB2: CS output high; PB3/4/5: SPI1 AF5 */
    GPIOB->BSRR = (1UL << 2U);
    GPIOB->MODER = (GPIOB->MODER & ~((3UL << 4U) | (3UL << 6U) | (3UL << 8U) | (3UL << 10U))) |
                   (1UL << 4U) | (2UL << 6U) | (2UL << 8U) | (2UL << 10U);
    GPIOB->OSPEEDR |= (3UL << 4U) | (3UL << 6U) | (3UL << 8U) | (3UL << 10U);
    GPIOB->PUPDR = (GPIOB->PUPDR & ~((3UL << 6U) | (3UL << 8U) | (3UL << 10U))) |
                   (1UL << 6U) | (1UL << 8U) | (1UL << 10U);
    GPIOB->AFRL = (GPIOB->AFRL & ~((0xFUL << 12U) | (0xFUL << 16U) | (0xFUL << 20U))) |
                  (5UL << 12U) | (5UL << 16U) | (5UL << 20U);

    /* SPI1 Master, Mode 3 (CPOL=1, CPHA=1), Soft NSS, Prescaler=4 (21 MHz) */
    SPI1->CR1 = (1UL << 9U) | (1UL << 8U) | (1UL << 3U) | (1UL << 2U) |
                (1UL << 1U) | (1UL << 0U) | (1UL << 6U);
}

static void w25q_wait_busy(void)
{
    uint8_t sr;
    do {
        w25q_cs_low();
        (void)w25q_xfer(W25Q_CMD_READ_STATUS1);
        sr = w25q_xfer(0xFFU);
        w25q_cs_high();
    } while ((sr & 0x01U) != 0U);
}

static void w25q_read(uint32_t addr, void *buf, uint32_t len)
{
    uint8_t *p = (uint8_t *)buf;
    w25q_wait_busy();
    w25q_cs_low();
    (void)w25q_xfer(W25Q_CMD_READ_DATA);
    (void)w25q_xfer((uint8_t)(addr >> 16U));
    (void)w25q_xfer((uint8_t)(addr >> 8U));
    (void)w25q_xfer((uint8_t)addr);
    while (len-- > 0UL) {
        *p++ = w25q_xfer(0xFFU);
    }
    w25q_cs_high();
}

/* =========================================================================
 * STM32F401 Internal Flash Erase & Programming (Sectors 1..5 + Sector 0 RAMFunc)
 * ========================================================================= */
static void stm32_flash_unlock(void)
{
    if ((FLASH_CR & FLASH_CR_LOCK_BIT) != 0UL) {
        FLASH_KEYR = FLASH_KEY1;
        FLASH_KEYR = FLASH_KEY2;
    }
}

static void stm32_flash_lock_and_flush(void)
{
    /* Reset ART instruction & data caches so subsequent CRC reads fresh Flash */
    FLASH_ACR &= ~((1UL << 9U) | (1UL << 10U));
    FLASH_ACR |= (1UL << 11U) | (1UL << 12U);
    FLASH_ACR &= ~((1UL << 11U) | (1UL << 12U));
    FLASH_ACR |= (1UL << 9U) | (1UL << 10U);
    FLASH_CR |= FLASH_CR_LOCK_BIT;
}

static int stm32_flash_erase_sector(uint32_t sector_num)
{
    while ((FLASH_SR & FLASH_SR_BSY_BIT) != 0UL) {
    }
    FLASH_SR = FLASH_SR_ERR_MASK | FLASH_SR_EOP_BIT;
    FLASH_CR = FLASH_CR_PSIZE_WORD | FLASH_CR_SER_BIT | (sector_num << 3U);
    FLASH_CR |= FLASH_CR_STRT_BIT;
    __asm__ volatile ("dsb" ::: "memory");
    while ((FLASH_SR & FLASH_SR_BSY_BIT) != 0UL) {
    }
    FLASH_CR &= ~(FLASH_CR_SER_BIT | (0xFUL << 3U));
    return ((FLASH_SR & FLASH_SR_ERR_MASK) == 0UL) ? 0 : -1;
}

static int stm32_flash_program_words(uint32_t dst_addr, const uint8_t *src, uint32_t len)
{
    uint32_t i;
    for (i = 0UL; i < len; i += 4UL) {
        uint32_t word = 0xFFFFFFFFUL;
        uint32_t rem = len - i;

        bl_memcpy(&word, src + i, (rem >= 4UL) ? 4U : rem);
        while ((FLASH_SR & FLASH_SR_BSY_BIT) != 0UL) {
        }
        FLASH_SR = FLASH_SR_ERR_MASK | FLASH_SR_EOP_BIT;
        FLASH_CR = FLASH_CR_PSIZE_WORD | FLASH_CR_PG_BIT;
        REG32(dst_addr + i) = word;
        __asm__ volatile ("dsb" ::: "memory");
        while ((FLASH_SR & FLASH_SR_BSY_BIT) != 0UL) {
        }
        FLASH_CR &= ~FLASH_CR_PG_BIT;
        if ((FLASH_SR & FLASH_SR_ERR_MASK) != 0UL) {
            return -1;
        }
    }
    return 0;
}

/*
 * SRAM-resident self-upgrade routine for Sector 0 (0x08000000 - 0x08003FFF).
 * Executes entirely from `.ramfunc` in SRAM while erasing and reprogramming
 * Sector 0, then triggers a system reset into the newly flashed Bootloader.
 */
__attribute__((section(".ramfunc"), noinline, long_call, noreturn))
static void bl_ramfunc_flash_sector0_and_reset(const uint8_t *ram_img, uint32_t size)
{
    uint32_t i;

    if ((FLASH_CR & FLASH_CR_LOCK_BIT) != 0UL) {
        FLASH_KEYR = FLASH_KEY1;
        FLASH_KEYR = FLASH_KEY2;
    }
    while ((FLASH_SR & FLASH_SR_BSY_BIT) != 0UL) {
    }
    FLASH_SR = FLASH_SR_ERR_MASK | FLASH_SR_EOP_BIT;

    /* Erase Sector 0 (snb = 0) */
    FLASH_CR = FLASH_CR_PSIZE_WORD | FLASH_CR_SER_BIT | (0UL << 3U);
    FLASH_CR |= FLASH_CR_STRT_BIT;
    __asm__ volatile ("dsb" ::: "memory");
    while ((FLASH_SR & FLASH_SR_BSY_BIT) != 0UL) {
    }
    FLASH_CR &= ~(FLASH_CR_SER_BIT | (0xFUL << 3U));

    /* Program Sector 0 word-by-word from SRAM buffer */
    for (i = 0UL; i < size; i += 4UL) {
        uint32_t word = 0xFFFFFFFFUL;
        uint32_t k;
        for (k = 0UL; k < 4UL && (i + k) < size; k++) {
            word = (word & ~(0xFFUL << (k * 8U))) |
                   ((uint32_t)ram_img[i + k] << (k * 8U));
        }
        FLASH_SR = FLASH_SR_ERR_MASK | FLASH_SR_EOP_BIT;
        FLASH_CR = FLASH_CR_PSIZE_WORD | FLASH_CR_PG_BIT;
        REG32(NK60_BOOTLOADER_FLASH_BASE + i) = word;
        __asm__ volatile ("dsb" ::: "memory");
        while ((FLASH_SR & FLASH_SR_BSY_BIT) != 0UL) {
        }
        FLASH_CR &= ~FLASH_CR_PG_BIT;
    }

    FLASH_CR |= FLASH_CR_LOCK_BIT;
    SCB_AIRCR = 0x05FA0004UL;
    for (;;) {
    }
}

/* =========================================================================
 * W25QXX Staged Image Verification & Internal Flash Upgrade Routines
 * ========================================================================= */
static bool verify_w25q_image(uint32_t w25q_base, uint8_t exp_type,
                              uint32_t max_payload, uint32_t flash_start,
                              uint32_t flash_end, struct nlk_fw_img_hdr *hdr_out,
                              uint32_t *calc_crc_out)
{
    struct nlk_fw_img_hdr hdr;
    uint32_t vec[16];
    uint32_t off = 0UL;
    uint32_t crc = 0UL;

    w25q_read(w25q_base, &hdr, sizeof(hdr));
    if (hdr.magic != NLK_FW_IMG_MAGIC || hdr.type != exp_type ||
        hdr.payload_crc32 == 0UL ||
        hdr.size < sizeof(vec) || hdr.size > max_payload) {
        return false;
    }

    w25q_read(w25q_base + NK60_IMG_HDR_SIZE, vec, sizeof(vec));
    if (!nk60_validate_vector_table(vec, flash_start, flash_end)) {
        return false;
    }

    while (off < hdr.size) {
        uint32_t chunk = hdr.size - off;
        if (chunk > sizeof(g_io_buf)) {
            chunk = sizeof(g_io_buf);
        }
        w25q_read(w25q_base + NK60_IMG_HDR_SIZE + off, g_io_buf, chunk);
        crc = bl_crc32(crc, g_io_buf, chunk);
        off += chunk;
    }

    if (crc != hdr.payload_crc32) {
        return false;
    }
    *hdr_out = hdr;
    *calc_crc_out = crc;
    return true;
}

static void handle_bootloader_self_upgrade(void)
{
    struct nlk_fw_img_hdr hdr;
    uint32_t w25q_crc = 0UL;
    uint32_t int_crc;
    uint32_t ram_crc;

    if (!verify_w25q_image(NK60_W25Q_BOOTLOADER_ADDR, NLK_FW_IMG_TYPE_BOOTLOADER,
                           NK60_BOOTLOADER_MAX_BIN, NK60_BOOTLOADER_FLASH_BASE,
                           NK60_BOOTLOADER_FLASH_BASE + NK60_BOOTLOADER_MAX_BIN,
                           &hdr, &w25q_crc)) {
        uart_puts("[BL] Invalid staged bootloader in W25QXX, skipping\r\n");
        RTC_BKP0R = NK60_BOOT_BKP_NORMAL;
        return;
    }

    /* If Sector 0 already contains this exact bootloader image, just clear pending flag */
    int_crc = bl_crc32(0UL, (const void *)(uintptr_t)NK60_BOOTLOADER_FLASH_BASE, hdr.size);
    if (int_crc == w25q_crc) {
        uart_puts("[BL] Sector 0 bootloader already up-to-date\r\n");
        RTC_BKP0R = NK60_BOOT_BKP_NORMAL;
        return;
    }

    uart_puts("[BL] Upgrading Sector 0 Bootloader from W25QXX...\r\n");
    rgb_set_solid(0U, 0U, 255U);
    w25q_read(NK60_W25Q_BOOTLOADER_ADDR + NK60_IMG_HDR_SIZE, g_bl_ram_buf, hdr.size);
    ram_crc = bl_crc32(0UL, g_bl_ram_buf, hdr.size);
    if (ram_crc != w25q_crc) {
        rgb_set_solid(255U, 0U, 0U);
        return;
    }

    RTC_BKP0R = NK60_BOOT_BKP_NORMAL;
    bl_ramfunc_flash_sector0_and_reset(g_bl_ram_buf, hdr.size);
}

static int install_kernel(void)
{
    /* STM32F401 Sectors 1..5 end offsets relative to 0x08004000 */
    static const uint32_t sec_end_offsets[5] = {
        16UL * 1024UL,   /* Sector 1: 0x08004000 - 0x08007FFF (16 KB) */
        32UL * 1024UL,   /* Sector 2: 0x08008000 - 0x0800BFFF (16 KB) */
        48UL * 1024UL,   /* Sector 3: 0x0800C000 - 0x0800FFFF (16 KB) */
        112UL * 1024UL,  /* Sector 4: 0x08010000 - 0x0801FFFF (64 KB) */
        240UL * 1024UL,  /* Sector 5: 0x08020000 - 0x0803FFFF (128 KB) */
    };
    uint32_t w25q_base = NK60_W25Q_KERNEL_ADDR;
    struct nlk_fw_img_hdr hdr;
    uint32_t exp_crc = 0UL;
    uint32_t int_crc;
    uint32_t first_word = 0xFFFFFFFFUL;
    uint32_t off = 4UL;
    uint32_t s;

    uart_puts("[BL] Verifying Kernel image in W25QXX...\r\n");

    if (!verify_w25q_image(w25q_base, NLK_FW_IMG_TYPE_KERNEL,
                           NK60_KERNEL_MAX_BIN - NK60_IMG_HDR_SIZE,
                           NK60_KERNEL_FLASH_BASE, NK60_INT_FLASH_END,
                           &hdr, &exp_crc)) {
        uart_puts("[BL] Staged kernel image invalid or CRC mismatch!\r\n");
        return -1;
    }

    rgb_set_solid(0U, 0U, 255U);
    w25q_read(w25q_base + NK60_IMG_HDR_SIZE, &first_word, sizeof(first_word));

    uart_puts("[BL] Erasing & programming internal Flash (size=");
    uart_put_hex(hdr.size);
    uart_puts(")...\r\n");

    stm32_flash_unlock();
    for (s = 0UL; s < 5UL; s++) {
        uint32_t prev_end = (s == 0UL) ? 0UL : sec_end_offsets[s - 1UL];
        if (hdr.size > prev_end) {
            if (stm32_flash_erase_sector(s + 1UL) != 0) {
                stm32_flash_lock_and_flush();
                return -1;
            }
        }
    }

    /*
     * Power-loss atomic commit without external Flash metadata wear:
     * Leave word 0 (MSP at 0x08004000) erased (0xFFFFFFFF) while programming
     * [0x08004004 .. 0x08004000 + hdr.size), then verify CRC32, and finally
     * program word 0 last. Any interruption leaves 0x08004000 == 0xFFFFFFFF so
     * the next boot automatically restores from W25QXX.
     */
    while (off < hdr.size) {
        uint32_t chunk = hdr.size - off;
        if (chunk > sizeof(g_io_buf)) {
            chunk = sizeof(g_io_buf);
        }
        w25q_read(w25q_base + NK60_IMG_HDR_SIZE + off, g_io_buf, chunk);
        if (stm32_flash_program_words(NK60_KERNEL_FLASH_BASE + off,
                                      g_io_buf, chunk) != 0) {
            stm32_flash_lock_and_flush();
            return -1;
        }
        off += chunk;
    }
    stm32_flash_lock_and_flush();

    int_crc = bl_crc32(0UL, &first_word, sizeof(first_word));
    if (hdr.size > 4UL) {
        int_crc = bl_crc32(int_crc,
                           (const void *)(uintptr_t)(NK60_KERNEL_FLASH_BASE + 4UL),
                           hdr.size - 4UL);
    }
    if (int_crc != exp_crc) {
        uart_puts("[BL] Internal Flash CRC mismatch!\r\n");
        return -1;
    }

    stm32_flash_unlock();
    if (stm32_flash_program_words(NK60_KERNEL_FLASH_BASE,
                                  (const uint8_t *)&first_word,
                                  sizeof(first_word)) != 0) {
        stm32_flash_lock_and_flush();
        return -1;
    }
    stm32_flash_lock_and_flush();

    RTC_BKP0R = NK60_BOOT_BKP_NORMAL;

    rgb_set_solid(0U, 255U, 0U);
    uart_puts("[BL] Kernel upgrade verified OK (crc=");
    uart_put_hex(exp_crc);
    uart_puts(")\r\n");
    return 0;
}

static bool is_internal_kernel_valid(void)
{
    const uint32_t *vec = (const uint32_t *)(uintptr_t)NK60_KERNEL_FLASH_BASE;

    return nk60_validate_vector_table(vec,
                                      NK60_KERNEL_FLASH_BASE, NK60_INT_FLASH_END);
}

__attribute__((noreturn))
static void jump_to_kernel(uint32_t app_base)
{
    uint32_t app_msp = REG32(app_base);
    uint32_t app_entry = REG32(app_base + 4UL);
    uint32_t i;

    __asm__ volatile ("cpsid i" ::: "memory");

    /* Stop SysTick and clear all NVIC enables/pendings */
    SYSTICK_CSR = 0UL;
    for (i = 0UL; i < 8UL; i++) {
        REG32(NVIC_ICER_BASE + i * 4UL) = 0xFFFFFFFFUL;
        REG32(NVIC_ICPR_BASE + i * 4UL) = 0xFFFFFFFFUL;
    }

    /* Wait for final UART TX byte before handing over console to NOS */
    while ((USART1->SR & (1UL << 6U)) == 0UL) {
    }

    /* Reset SPI1 and SPI2 peripherals so NOS drivers start from clean state */
    RCC_APB2RSTR |= (1UL << 12U);
    RCC_APB2RSTR &= ~(1UL << 12U);
    RCC_APB1RSTR |= (1UL << 14U);
    RCC_APB1RSTR &= ~(1UL << 14U);

    SCB_VTOR = app_base;
    __asm__ volatile (
        "msr msp, %0\n\t"
        "dsb\n\t"
        "isb\n\t"
        "bx  %1\n\t"
        :
        : "r"(app_msp), "r"(app_entry)
        : "memory"
    );
    for (;;) {
    }
}

int main(void)
{
    uint32_t bkp_cmd;
    uint8_t key_cmd;
    uint8_t cmd = NK60_BOOT_CMD_NONE;

    clock_init_84mhz();
    uart1_init();
    w25q_init();
    rgb_init();

    uart_puts("\r\n[BL] NK60-V2 Bootloader v1.0.0\r\n");

    bkp_cmd = RTC_BKP0R;
    key_cmd = key_scan_recovery_cmd();

    if (key_cmd != NK60_BOOT_CMD_NONE) {
        uart_puts("[BL] Physical recovery key combo detected\r\n");
        cmd = key_cmd;
    } else if (bkp_cmd == NK60_BOOT_CMD_UPDATE_BL) {
        handle_bootloader_self_upgrade();
    } else if (bkp_cmd == NK60_BOOT_CMD_UPDATE_KERNEL) {
        cmd = NK60_BOOT_CMD_UPDATE_KERNEL;
    }

    if (cmd == NK60_BOOT_CMD_UPDATE_KERNEL) {
        if (install_kernel() != 0) {
            rgb_set_solid(255U, 0U, 0U);
            uart_puts("[BL] Staged kernel invalid, keeping internal Flash kernel\r\n");
            RTC_BKP0R = NK60_BOOT_BKP_NORMAL;
        }
    } else if (bkp_cmd != NK60_BOOT_BKP_NORMAL) {
        RTC_BKP0R = NK60_BOOT_BKP_NORMAL;
    }

    if (!is_internal_kernel_valid()) {
        uart_puts("[BL] Internal kernel invalid, restoring from W25QXX...\r\n");
        (void)install_kernel();
    }

    if (is_internal_kernel_valid()) {
        uart_puts("[BL] Booting NOS @ 0x08004000\r\n");
        jump_to_kernel(NK60_KERNEL_FLASH_BASE);
    }

    rgb_set_solid(255U, 0U, 0U);
    uart_puts("[BL] FATAL: No bootable NOS kernel found!\r\n");
    for (;;) {
    }
}
