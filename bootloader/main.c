/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 独立 Bootloader (STM32F401CCU6, Sector 0: 0x08000000 - 0x08003FFF, 16 KB)。
 *
 * 核心特性：
 * - 完全移除外部 W25QXX SPI Flash 依赖，直接擦写 STM32 内部 Flash Sector 1..5 (0x08004000, 240 KB)。
 * - 统一识别内核 <kernel/boot.h> 的 BOOT_FLAG_MAGIC | BOOT_BOOTLOADER (0x20300001) 标志。
 * - 内置裸机轮询式 USB OTG FS WinUSB (0x0EFF:0x0001) 与 UART1 NLK 协议栈，原生支持
 *   `nlk flash kernel` 直接刷写片上 Flash 及 `nlk reboot` 重启引导。
 * - 掉电安全提交：刷写过程中暂存 0x08004000 首字 (MSP)，待全量 CRC32 校验通过后再落盘首字。
 */

#include <boot_proto.h>

/* =========================================================================
 * STM32F401 寄存器定义（零外部库依赖）
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
#define RCC_APB1RSTR                REG32(RCC_BASE + 0x20UL)
#define RCC_APB2RSTR                REG32(RCC_BASE + 0x24UL)
#define RCC_AHB1ENR                 REG32(RCC_BASE + 0x30UL)
#define RCC_AHB2ENR                 REG32(RCC_BASE + 0x34UL)
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

/* USART1 (0x40011000) & SPI2 (0x40003800) */
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
#define SPI2                        ((struct stm32_spi_regs *)0x40003800UL)

/* USB OTG FS (0x50000000) */
#define USB_OTG_BASE                0x50000000UL
#define OTG_GOTGCTL                 REG32(USB_OTG_BASE + 0x000UL)
#define OTG_GAHBCFG                 REG32(USB_OTG_BASE + 0x008UL)
#define OTG_GUSBCFG                 REG32(USB_OTG_BASE + 0x00CUL)
#define OTG_GRSTCTL                 REG32(USB_OTG_BASE + 0x010UL)
#define OTG_GINTSTS                 REG32(USB_OTG_BASE + 0x014UL)
#define OTG_GINTMSK                 REG32(USB_OTG_BASE + 0x018UL)
#define OTG_GRXSTSP                 REG32(USB_OTG_BASE + 0x020UL)
#define OTG_GRXFSIZ                 REG32(USB_OTG_BASE + 0x024UL)
#define OTG_DIEPTXF0                REG32(USB_OTG_BASE + 0x028UL)
#define OTG_GCCFG                   REG32(USB_OTG_BASE + 0x038UL)
#define OTG_DIEPTXF(n)              REG32(USB_OTG_BASE + 0x100UL + ((n) * 4UL))
#define OTG_DCFG                    REG32(USB_OTG_BASE + 0x800UL)
#define OTG_DCTL                    REG32(USB_OTG_BASE + 0x804UL)
#define OTG_DIEPMSK                 REG32(USB_OTG_BASE + 0x810UL)
#define OTG_DOEPMSK                 REG32(USB_OTG_BASE + 0x814UL)
#define OTG_DAINTMSK                REG32(USB_OTG_BASE + 0x81CUL)
#define OTG_DIEPCTL(n)              REG32(USB_OTG_BASE + 0x900UL + ((n) * 0x20UL))
#define OTG_DIEPINT(n)              REG32(USB_OTG_BASE + 0x908UL + ((n) * 0x20UL))
#define OTG_DIEPTSIZ(n)             REG32(USB_OTG_BASE + 0x910UL + ((n) * 0x20UL))
#define OTG_DOEPCTL(n)              REG32(USB_OTG_BASE + 0xB00UL + ((n) * 0x20UL))
#define OTG_DOEPINT(n)              REG32(USB_OTG_BASE + 0xB08UL + ((n) * 0x20UL))
#define OTG_DOEPTSIZ(n)             REG32(USB_OTG_BASE + 0xB10UL + ((n) * 0x20UL))
#define OTG_FIFO(n)                 REG32(USB_OTG_BASE + 0x1000UL + ((n) * 0x1000UL))

/* NK60-V2 74HC595 按键矩阵恢复组合键位索引 */
#define NK60_RAW_BITS               72U
#define NK60_RAW_BIT_SPACE          51U
#define NK60_RAW_BIT_F              68U
#define NK60_RAW_BIT_J              71U

#define BL_VERSION_CODE \
    (((uint32_t)(BL_VER_MAJOR) << 16U) | ((uint32_t)(BL_VER_MINOR) << 8U) | (uint32_t)(BL_VER_PATCH))

#define BL_NLK_MTU                  512U

/* =========================================================================
 * 基础工具函数与紧凑型 CRC8 / CRC16 / CRC32
 * ========================================================================= */
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

static void bl_memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n-- > 0U) {
        *d++ = *s++;
    }
}

static void bl_memset(void *dst, uint8_t val, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n-- > 0U) {
        *d++ = val;
    }
}

static uint8_t bl_crc8(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint8_t crc = 0x00U;
    size_t i;
    uint8_t b;

    for (i = 0U; i < len; i++) {
        crc ^= p[i];
        for (b = 0U; b < 8U; b++) {
            crc = ((crc & 0x80U) != 0U) ? (uint8_t)((crc << 1U) ^ 0x07U) : (uint8_t)(crc << 1U);
        }
    }
    return crc;
}

static uint16_t bl_crc16(const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    uint16_t crc = 0xFFFFU;
    size_t i;
    uint8_t b;

    for (i = 0U; i < len; i++) {
        crc ^= (uint16_t)((uint16_t)p[i] << 8U);
        for (b = 0U; b < 8U; b++) {
            crc = ((crc & 0x8000U) != 0U) ? (uint16_t)((crc << 1U) ^ 0x1021U) : (uint16_t)(crc << 1U);
        }
    }
    return crc;
}

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

/* =========================================================================
 * 时钟 (84 MHz SYSCLK + 48 MHz USBCLK)、串口1 与 RGB 状态指示灯
 * ========================================================================= */
static void clock_init_84mhz(void)
{
    uint32_t timeout = 0x5000UL;

    SCB_CPACR |= (3UL << 20U) | (3UL << 22U);
    RCC_CR |= (1UL << 0U);
    RCC_CFGR = 0x00000000UL;
    RCC_CR &= ~((1UL << 16U) | (1UL << 18U) | (1UL << 24U));
    RCC_CIR = 0x00000000UL;

    RCC_APB1ENR |= (1UL << 28U);
    PWR_CR |= (2UL << 14U) | (1UL << 8U); /* 解锁 RTC_BKP0R 备份域访问 */

    RCC_CR |= (1UL << 16U);
    while (((RCC_CR & (1UL << 17U)) == 0UL) && (--timeout > 0UL)) {
    }

    FLASH_ACR = (2UL << 0U) | (1UL << 8U) | (1UL << 9U) | (1UL << 10U);
    RCC_CFGR |= (4UL << 10U);

    if (timeout > 0UL) {
        /* 8 MHz HSE: M=8, N=336, P=4 (84 MHz), Q=7 (48 MHz USB) */
        RCC_PLLCFGR = 8UL | (336UL << 6U) | (1UL << 16U) | (1UL << 22U) | (7UL << 24U);
    } else {
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
    RCC_AHB1ENR |= (1UL << 0U);
    RCC_APB2ENR |= (1UL << 4U);

    /* PA9 (TX) & PA10 (RX) -> AF7 */
    GPIOA->MODER = (GPIOA->MODER & ~((3UL << 18U) | (3UL << 20U))) |
                   (2UL << 18U) | (2UL << 20U);
    GPIOA->OSPEEDR |= (3UL << 18U) | (3UL << 20U);
    GPIOA->PUPDR = (GPIOA->PUPDR & ~((3UL << 18U) | (3UL << 20U))) |
                   (1UL << 18U) | (1UL << 20U);
    GPIOA->AFRH = (GPIOA->AFRH & ~((0xFUL << 4U) | (0xFUL << 8U))) |
                  (7UL << 4U) | (7UL << 8U);

    USART1->BRR = 0x02D9UL; /* 84 MHz / 115200 */
    USART1->CR1 = (1UL << 13U) | (1UL << 3U) | (1UL << 2U); /* UE | TE | RE */
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

static void rgb_init(void)
{
    RCC_AHB1ENR |= (1UL << 1U) | (1UL << 2U);
    RCC_APB1ENR |= (1UL << 14U);

    GPIOB->MODER = (GPIOB->MODER & ~(3UL << 0U)) | (1UL << 0U);
    GPIOB->BSRR = (1UL << 16U);

    GPIOC->MODER = (GPIOC->MODER & ~(3UL << 6U)) | (2UL << 6U);
    GPIOC->OSPEEDR |= (3UL << 6U);
    GPIOC->AFRL = (GPIOC->AFRL & ~(0xFUL << 12U)) | (5UL << 12U);

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
    GPIOB->BSRR = (1UL << 0U);
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

static bool key_scan_recovery_pressed(void)
{
    uint32_t i;
    bool f_down = false;
    bool j_down = false;
    bool spc_down = false;

    RCC_AHB1ENR |= (1UL << 1U);
    GPIOB->MODER = (GPIOB->MODER & ~((3UL << 24U) | (3UL << 26U) | (3UL << 28U) | (3UL << 30U))) |
                   (1UL << 24U) | (1UL << 26U) | (1UL << 30U);
    GPIOB->PUPDR = (GPIOB->PUPDR & ~(3UL << 28U)) | (1UL << 28U);
    GPIOB->BSRR = (1UL << (12U + 16U)) | (1UL << (13U + 16U)) | (1UL << (15U + 16U));
    delay_us(10U);

    for (i = 0UL; i < NK60_RAW_BITS; i++) {
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
    return (spc_down && (f_down || j_down));
}

/* =========================================================================
 * STM32F401 片上内部 Flash 擦除与编程 (Sectors 1..5: 0x08004000 - 0x0803FFFF)
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

static bool is_internal_kernel_valid(void)
{
    const uint32_t *vec = (const uint32_t *)(uintptr_t)NK60_KERNEL_FLASH_BASE;
    return nk60_validate_vector_table(vec, NK60_KERNEL_FLASH_BASE, NK60_INT_FLASH_END);
}

__attribute__((noreturn))
static void jump_to_kernel(uint32_t app_base)
{
    uint32_t app_msp = REG32(app_base);
    uint32_t app_entry = REG32(app_base + 4UL);
    uint32_t i;

    __asm__ volatile ("cpsid i" ::: "memory");
    SYSTICK_CSR = 0UL;
    for (i = 0UL; i < 8UL; i++) {
        REG32(NVIC_ICER_BASE + i * 4UL) = 0xFFFFFFFFUL;
        REG32(NVIC_ICPR_BASE + i * 4UL) = 0xFFFFFFFFUL;
    }

    while ((USART1->SR & (1UL << 6U)) == 0UL) {
    }

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

/* =========================================================================
 * 裸机 NLK Flash 会话与协议处理（直接擦写 STM32 内部 Flash Sector 1..5）
 * ========================================================================= */
static struct {
    bool     active;
    bool     reboot_pending;
    uint32_t reboot_flag;
    uint32_t total_size;
    uint32_t expected_crc32;
    uint32_t running_crc32;
    uint32_t acked_offset;
    uint32_t first_word;
} g_bl_flash;

static int bl_kernel_prepare(uint32_t total_size, uint32_t expected_crc32)
{
    static const uint32_t sec_end_offsets[5] = {
        16UL * 1024UL,   /* Sector 1: 16 KB */
        32UL * 1024UL,   /* Sector 2: 16 KB */
        48UL * 1024UL,   /* Sector 3: 16 KB */
        112UL * 1024UL,  /* Sector 4: 64 KB */
        240UL * 1024UL,  /* Sector 5: 128 KB */
    };
    uint32_t s;

    if (total_size < 64UL || total_size > NK60_KERNEL_MAX_BIN) {
        return -27; /* -EFBIG */
    }

    rgb_set_solid(0U, 0U, 255U);
    stm32_flash_unlock();
    for (s = 0UL; s < 5UL; s++) {
        uint32_t prev_end = (s == 0UL) ? 0UL : sec_end_offsets[s - 1UL];
        if (total_size > prev_end) {
            if (stm32_flash_erase_sector(s + 1UL) != 0) {
                stm32_flash_lock_and_flush();
                return -5; /* -EIO */
            }
        }
    }
    stm32_flash_lock_and_flush();

    g_bl_flash.active = true;
    g_bl_flash.total_size = total_size;
    g_bl_flash.expected_crc32 = expected_crc32;
    g_bl_flash.running_crc32 = 0UL;
    g_bl_flash.acked_offset = 0UL;
    g_bl_flash.first_word = 0xFFFFFFFFUL;
    return 0;
}

static int bl_kernel_write_chunk(uint32_t offset, const uint8_t *data, uint16_t len,
                                 struct nlk_flash_chunk_ack *ack)
{
    bl_memset(ack, 0, sizeof(*ack));
    if (!g_bl_flash.active) {
        ack->status = -1;
        return -1;
    }
    if (offset < g_bl_flash.acked_offset) {
        ack->acked_offset = g_bl_flash.acked_offset;
        ack->rx_window_bitmap = 1U;
        return 0;
    }
    if (offset != g_bl_flash.acked_offset ||
        offset + (uint32_t)len > g_bl_flash.total_size) {
        ack->acked_offset = g_bl_flash.acked_offset;
        ack->status = -22;
        return -22;
    }

    if (offset == 0UL && len >= 8U) {
        uint32_t msp;
        uint32_t reset_pc;

        bl_memcpy(&msp, data, 4U);
        bl_memcpy(&reset_pc, data + 4U, 4U);
        if (!nk60_validate_cortexm_vectors(msp, reset_pc,
                                           NK60_KERNEL_FLASH_BASE,
                                           NK60_INT_FLASH_END)) {
            ack->status = -74; /* -EBADMSG */
            return -74;
        }
        g_bl_flash.first_word = msp;
    }

    stm32_flash_unlock();
    if (offset == 0UL && len > 4U) {
        /* 首字 (MSP) 暂存于 RAM，先从偏移 4 开始写入内部 Flash 以保证掉电安全 */
        if (stm32_flash_program_words(NK60_KERNEL_FLASH_BASE + 4UL,
                                      data + 4U, (uint32_t)(len - 4U)) != 0) {
            stm32_flash_lock_and_flush();
            ack->status = -5;
            return -5;
        }
    } else if (offset > 0UL && len > 0U) {
        if (stm32_flash_program_words(NK60_KERNEL_FLASH_BASE + offset,
                                      data, len) != 0) {
            stm32_flash_lock_and_flush();
            ack->status = -5;
            return -5;
        }
    }
    stm32_flash_lock_and_flush();

    g_bl_flash.running_crc32 = bl_crc32(g_bl_flash.running_crc32, data, len);
    g_bl_flash.acked_offset += len;
    ack->acked_offset = g_bl_flash.acked_offset;
    ack->rx_window_bitmap = 1U;
    ack->status = 0;
    return 0;
}

static int bl_kernel_commit(void)
{
    uint32_t int_crc;

    if (!g_bl_flash.active || g_bl_flash.acked_offset != g_bl_flash.total_size) {
        return -22;
    }

    stm32_flash_lock_and_flush();
    int_crc = bl_crc32(0UL, &g_bl_flash.first_word, sizeof(g_bl_flash.first_word));
    if (g_bl_flash.total_size > 4UL) {
        int_crc = bl_crc32(int_crc,
                           (const void *)(uintptr_t)(NK60_KERNEL_FLASH_BASE + 4UL),
                           g_bl_flash.total_size - 4UL);
    }

    if ((g_bl_flash.expected_crc32 != 0UL && int_crc != g_bl_flash.expected_crc32) ||
        int_crc != g_bl_flash.running_crc32) {
        g_bl_flash.active = false;
        rgb_set_solid(255U, 0U, 0U);
        return -74; /* -EBADMSG */
    }

    /* 全文 CRC32 校验通过后原子写入首字 MSP (0x08004000) */
    stm32_flash_unlock();
    if (stm32_flash_program_words(NK60_KERNEL_FLASH_BASE,
                                  (const uint8_t *)&g_bl_flash.first_word,
                                  sizeof(g_bl_flash.first_word)) != 0) {
        stm32_flash_lock_and_flush();
        return -5;
    }
    stm32_flash_lock_and_flush();

    g_bl_flash.active = false;
    rgb_set_solid(0U, 255U, 0U);
    uart_puts("[BL] Kernel flashed to internal Flash OK\r\n");
    return 0;
}

static void bl_fill_flash_query(struct nlk_flash_query_resp *q)
{
    bl_memset(q, 0, sizeof(*q));
    q->part_count = 2U;
    q->run_mode = BOOT_BOOTLOADER;
    q->max_chunk_size = 256U;
    q->running_version = BL_VERSION_CODE;

    q->parts[0].part_id = 0U;
    q->parts[0].flags = 0U; /* Bootloader 自身在 BL 模式下只读 */
    q->parts[0].max_size = NK60_BOOTLOADER_MAX_BIN;
    q->parts[0].erase_size = NK60_BOOTLOADER_MAX_BIN;
    bl_memcpy(q->parts[0].name, "bootloader", 11U);

    q->parts[1].part_id = 1U;
    q->parts[1].flags = NLK_FLASH_PART_F_WRITABLE | NLK_FLASH_PART_F_REBOOT_AFTER;
    q->parts[1].max_size = NK60_KERNEL_MAX_BIN;
    q->parts[1].erase_size = 16UL * 1024UL;
    bl_memcpy(q->parts[1].name, "kernel", 7U);
}

static uint16_t bl_handle_nlk_packet(const uint8_t *rx_pkt, uint16_t rx_len,
                                     uint8_t *tx_pkt)
{
    const struct nlk_pkt_hdr *in_hdr = (const struct nlk_pkt_hdr *)rx_pkt;
    const uint8_t *payload = rx_pkt + sizeof(*in_hdr);
    uint16_t payload_len;
    struct nlk_pkt_hdr *out_hdr = (struct nlk_pkt_hdr *)tx_pkt;
    uint8_t *out_pay = tx_pkt + sizeof(*out_hdr);
    uint16_t out_len = 0U;
    int rc = 0;
    uint8_t mode;

    if (rx_len < sizeof(*in_hdr)) {
        return 0U;
    }
    payload_len = in_hdr->payload_len;
    if ((uint32_t)sizeof(*in_hdr) + payload_len > rx_len) {
        return 0U;
    }
    mode = (uint8_t)((in_hdr->msg_type >> 4U) & 0x0FU);

    if (in_hdr->dst_cap == NLK_CAP_ID_CORE) {
        switch (in_hdr->cmd_op) {
        case NLK_CORE_OP_GET_DEV_DESC: {
            struct nlk_device_desc *d = (struct nlk_device_desc *)out_pay;
            bl_memset(d, 0, sizeof(*d));
            d->vendor_id = 0x4E4BU;
            d->product_id = 0x0060U;
            d->device_class = 0x0010U;
            d->hw_ver[0] = 2U;
            d->fw_ver[0] = (uint8_t)BL_VER_MAJOR;
            d->fw_ver[1] = (uint8_t)BL_VER_MINOR;
            d->fw_ver[2] = (uint8_t)BL_VER_PATCH;
            bl_memcpy(d->name, "NK60-V2-BL", 11U);
            d->max_payload_size = BL_NLK_MTU;
            d->cap_count = 1U;
            out_len = sizeof(*d);
            break;
        }
        case NLK_CORE_OP_GET_CAP_LIST: {
            struct nlk_cap_item *item = (struct nlk_cap_item *)out_pay;
            bl_memset(item, 0, sizeof(*item));
            item->cap_id = NLK_CAP_ID_FLASH;
            item->cap_ver = 0x10U;
            item->cap_type = NLK_CAP_FLASH;
            item->sup_modes = NLK_SUP_MODE_RPC | NLK_SUP_MODE_BULK;
            item->desc_size = sizeof(struct nlk_flash_query_resp);
            out_len = sizeof(*item);
            break;
        }
        case NLK_CORE_OP_GET_CAP_DESC:
            bl_fill_flash_query((struct nlk_flash_query_resp *)out_pay);
            out_len = sizeof(struct nlk_flash_query_resp);
            break;
        case NLK_CORE_OP_PING:
            bl_memcpy(out_pay, payload, payload_len);
            out_len = payload_len;
            break;
        case NLK_CORE_OP_RESET:
            g_bl_flash.active = false;
            break;
        case NLK_CORE_OP_REBOOT: {
            uint32_t flag = BOOT_NORMAL;
            if (payload_len >= sizeof(struct nlk_core_reboot_req)) {
                bl_memcpy(&flag, payload, sizeof(flag));
            }
            g_bl_flash.reboot_pending = true;
            g_bl_flash.reboot_flag = flag;
            break;
        }
        default:
            rc = -95;
            break;
        }
    } else if (in_hdr->dst_cap == NLK_CAP_ID_FLASH) {
        if (mode == NLK_MODE_BULK || in_hdr->cmd_op == NLK_FLASH_OP_WRITE_CHUNK) {
            struct nlk_flash_chunk_hdr chdr;
            struct nlk_flash_chunk_ack *ack = (struct nlk_flash_chunk_ack *)out_pay;
            if (payload_len < sizeof(chdr)) {
                rc = -22;
            } else {
                bl_memcpy(&chdr, payload, sizeof(chdr));
                rc = bl_kernel_write_chunk(chdr.offset,
                                           payload + sizeof(chdr),
                                           (uint16_t)(payload_len - sizeof(chdr)),
                                           ack);
                out_len = sizeof(*ack);
            }
        } else {
            switch (in_hdr->cmd_op) {
            case NLK_FLASH_OP_QUERY_PARTS:
                bl_fill_flash_query((struct nlk_flash_query_resp *)out_pay);
                out_len = sizeof(struct nlk_flash_query_resp);
                break;
            case NLK_FLASH_OP_PREPARE: {
                const struct nlk_flash_prepare_req *prep =
                    (const struct nlk_flash_prepare_req *)payload;
                if (payload_len < sizeof(*prep) || prep->part_id != 1U) {
                    rc = -22;
                } else {
                    rc = bl_kernel_prepare(prep->total_size, prep->expected_crc32);
                }
                break;
            }
            case NLK_FLASH_OP_COMMIT:
                rc = bl_kernel_commit();
                if (rc == 0) {
                    bl_memcpy(out_pay, &g_bl_flash.running_crc32, 4U);
                    out_len = 4U;
                }
                break;
            case NLK_FLASH_OP_ABORT:
                g_bl_flash.active = false;
                break;
            default:
                rc = -95;
                break;
            }
        }
    } else {
        rc = -2;
    }

    if ((in_hdr->ver_flags & NLK_FLAG_ACK_REQ) == 0U) {
        return 0U;
    }

    out_hdr->ver_flags = (uint8_t)(NLK_PROTO_VER_1 | NLK_FLAG_IS_ACK |
                                   ((rc < 0) ? NLK_FLAG_IS_ERR : 0U));
    out_hdr->msg_type = in_hdr->msg_type;
    out_hdr->src_addr = in_hdr->dst_addr;
    out_hdr->dst_addr = in_hdr->src_addr;
    out_hdr->dst_cap = in_hdr->dst_cap;
    out_hdr->cmd_op = in_hdr->cmd_op;
    out_hdr->seq_id = in_hdr->seq_id;

    if (rc < 0) {
        int16_t err16 = (int16_t)rc;
        bl_memcpy(out_pay, &err16, sizeof(err16));
        out_len = sizeof(err16);
    }
    out_hdr->payload_len = out_len;
    return (uint16_t)(sizeof(*out_hdr) + out_len);
}

/* =========================================================================
 * UART1 L2 Stream 通道收发 + 裸机 USB OTG FS WinUSB 轮询通道
 * ========================================================================= */
static uint8_t g_rx_pkt_buf[BL_NLK_MTU];
static uint8_t g_tx_pkt_buf[BL_NLK_MTU];

static void uart_send_nlk_frame(const uint8_t *pkt, uint16_t pkt_len)
{
    uint8_t len_b[2] = { (uint8_t)(pkt_len & 0xFFU), (uint8_t)(pkt_len >> 8U) };
    uint8_t crc8 = bl_crc8(len_b, 2U);
    uint16_t crc16 = bl_crc16(pkt, pkt_len);
    uint16_t i;

    uart_putc((char)NLK_SOF_BYTE0);
    uart_putc((char)NLK_SOF_BYTE1);
    uart_putc((char)len_b[0]);
    uart_putc((char)len_b[1]);
    uart_putc((char)crc8);
    for (i = 0U; i < pkt_len; i++) {
        uart_putc((char)pkt[i]);
    }
    uart_putc((char)(crc16 & 0xFFU));
    uart_putc((char)(crc16 >> 8U));
}

/* =========================================================================
 * 裸机 USB OTG FS WinUSB (0x0EFF:0x0001, EP1 Bulk OUT 0x01 / IN 0x81) 轮询驱动
 * ========================================================================= */
static const uint8_t g_usb_dev_desc[18] = {
    18, 0x01, 0x00, 0x02, 0x00, 0x00, 0x00, 64,
    0xFF, 0x0E, 0x01, 0x00, 0x00, 0x01, 1, 2, 3, 1
};

static const uint8_t g_usb_cfg_desc[32] = {
    9, 0x02, 32, 0x00, 1, 1, 0, 0x80, 250,
    9, 0x04, 0, 0, 2, 0xFF, 0x00, 0x00, 0,
    7, 0x05, 0x01, 0x02, 64, 0x00, 0,
    7, 0x05, 0x81, 0x02, 64, 0x00, 0
};

static const uint8_t g_usb_ms_os_str[18] = {
    18, 0x03, 'M',0,'S',0,'F',0,'T',0,'1',0,'0',0,'0',0, 'A', 0x00
};

static const uint8_t g_usb_compat_id[40] = {
    40, 0, 0, 0, 0x00, 0x01, 0x04, 0x00, 1, 0,0,0,0,0,0,0,
    0, 1, 'W','I','N','U','S','B',0,0, 0,0,0,0,0,0,0,0, 0,0,0,0,0,0
};

static uint16_t g_usb_rx_offset;
static uint16_t g_usb_rx_expected;

static void usb_write_packet(uint8_t ep_num, const uint8_t *data, uint16_t len)
{
    uint16_t words = (uint16_t)((len + 3U) / 4U);
    uint16_t i;

    OTG_DIEPTSIZ(ep_num) = (1UL << 19U) | len;
    OTG_DIEPCTL(ep_num) |= (1UL << 26U) | (1UL << 31U); /* CNAK | EPENA */

    for (i = 0U; i < words; i++) {
        uint32_t w = 0UL;
        uint16_t rem = (uint16_t)(len - (i * 4U));
        bl_memcpy(&w, data + (i * 4U), (rem >= 4U) ? 4U : rem);
        OTG_FIFO(ep_num) = w;
    }
}

static void usb_send_bulk_in(const uint8_t *buf, uint16_t len)
{
    uint16_t off = 0U;
    while (off < len) {
        uint16_t chunk = (uint16_t)(len - off);
        if (chunk > 64U) {
            chunk = 64U;
        }
        usb_write_packet(1U, buf + off, chunk);
        off = (uint16_t)(off + chunk);
    }
}

static void usb_arm_ep_out(uint8_t ep_num)
{
    OTG_DOEPTSIZ(ep_num) = (1UL << 19U) | (3UL << 29U) | 64UL;
    OTG_DOEPCTL(ep_num) |= (1UL << 26U) | (1UL << 31U); /* CNAK | EPENA */
}

static void usb_handle_setup(const uint8_t *setup)
{
    uint8_t bmReq = setup[0];
    uint8_t bReq = setup[1];
    uint16_t wVal = (uint16_t)setup[2] | ((uint16_t)setup[3] << 8U);
    uint16_t wIdx = (uint16_t)setup[4] | ((uint16_t)setup[5] << 8U);
    uint16_t wLen = (uint16_t)setup[6] | ((uint16_t)setup[7] << 8U);

    if ((bmReq & 0x60U) == 0x00U) {
        if (bReq == 0x05U) { /* SET_ADDRESS */
            OTG_DCFG = (OTG_DCFG & ~(0x7FUL << 4U)) | (((uint32_t)wVal & 0x7FUL) << 4U);
            usb_write_packet(0U, (const uint8_t *)"", 0U);
            return;
        }
        if (bReq == 0x09U) { /* SET_CONFIGURATION */
            OTG_DIEPCTL(1U) = (1UL << 15U) | (2UL << 18U) | (1UL << 22U) | (1UL << 28U) | 64UL;
            OTG_DOEPCTL(1U) = (1UL << 15U) | (2UL << 18U) | (1UL << 28U) | 64UL;
            usb_arm_ep_out(1U);
            usb_write_packet(0U, (const uint8_t *)"", 0U);
            return;
        }
        if (bReq == 0x06U) { /* GET_DESCRIPTOR */
            uint8_t dtype = (uint8_t)(wVal >> 8U);
            uint8_t didx = (uint8_t)(wVal & 0xFFU);
            if (dtype == 0x01U) {
                uint16_t n = (wLen < sizeof(g_usb_dev_desc)) ? wLen : sizeof(g_usb_dev_desc);
                usb_write_packet(0U, g_usb_dev_desc, n);
                return;
            }
            if (dtype == 0x02U) {
                uint16_t n = (wLen < sizeof(g_usb_cfg_desc)) ? wLen : sizeof(g_usb_cfg_desc);
                usb_write_packet(0U, g_usb_cfg_desc, n);
                return;
            }
            if (dtype == 0x03U) {
                if (didx == 0xEEU) {
                    uint16_t n = (wLen < sizeof(g_usb_ms_os_str)) ? wLen : sizeof(g_usb_ms_os_str);
                    usb_write_packet(0U, g_usb_ms_os_str, n);
                    return;
                }
                static const uint8_t lang_desc[4] = { 4, 0x03, 0x09, 0x04 };
                static const uint8_t str_desc[22] = {
                    22, 0x03, 'N',0,'K',0,'6',0,'0',0,'-',0,'V',0,'2',0,'-',0,'B',0,'L',0
                };
                const uint8_t *p = (didx == 0U) ? lang_desc : str_desc;
                uint16_t plen = (didx == 0U) ? 4U : 22U;
                uint16_t n = (wLen < plen) ? wLen : plen;
                usb_write_packet(0U, p, n);
                return;
            }
        }
        usb_write_packet(0U, (const uint8_t *)"", 0U);
        return;
    }

    if (bReq == 'A' && wIdx == 0x0004U) {
        uint16_t n = (wLen < sizeof(g_usb_compat_id)) ? wLen : sizeof(g_usb_compat_id);
        usb_write_packet(0U, g_usb_compat_id, n);
        return;
    }
    usb_write_packet(0U, (const uint8_t *)"", 0U);
}

static void usb_otg_init(void)
{
    /* PA11 (DM) & PA12 (DP) -> AF10 (OTG_FS) */
    RCC_AHB1ENR |= (1UL << 0U);
    RCC_AHB2ENR |= (1UL << 7U);

    GPIOA->MODER = (GPIOA->MODER & ~((3UL << 22U) | (3UL << 24U))) |
                   (2UL << 22U) | (2UL << 24U);
    GPIOA->OSPEEDR |= (3UL << 22U) | (3UL << 24U);
    GPIOA->AFRH = (GPIOA->AFRH & ~((0xFUL << 12U) | (0xFUL << 16U))) |
                  (10UL << 12U) | (10UL << 16U);

    OTG_GUSBCFG |= (1UL << 6U) | (1UL << 30U); /* PHYSEL | FDMOD (Force Device) */
    delay_us(20000U);
    OTG_GCCFG = (1UL << 16U) | (1UL << 21U);   /* PWRDWN | NOVBUSSENS */
    OTG_DCFG = (3UL << 0U);                    /* Full-Speed 1.1 PHY */
    OTG_GRXFSIZ = 128UL;
    OTG_DIEPTXF0 = (64UL << 16U) | 128UL;
    OTG_DIEPTXF(1) = (128UL << 16U) | 192UL;
    OTG_DCTL &= ~(1UL << 1U);                  /* 清除 SDIS 连接上拉 */
}

static void usb_otg_poll(void)
{
    uint32_t gint = OTG_GINTSTS;

    if ((gint & (1UL << 12U)) != 0UL) { /* USBRST */
        OTG_GINTSTS = (1UL << 12U);
        OTG_DCFG &= ~(0x7FUL << 4U);
        OTG_DIEPCTL(0U) = (1UL << 15U); /* 64B MPS */
        OTG_DOEPCTL(0U) = (1UL << 15U);
        usb_arm_ep_out(0U);
        g_usb_rx_offset = 0U;
        g_usb_rx_expected = 0U;
    }

    if ((gint & (1UL << 13U)) != 0UL) { /* ENUMDNE */
        OTG_GINTSTS = (1UL << 13U);
        usb_arm_ep_out(0U);
    }

    if ((gint & (1UL << 4U)) != 0UL) { /* RXFLVL */
        uint32_t st = OTG_GRXSTSP;
        uint8_t ep = (uint8_t)(st & 0x0FU);
        uint16_t bcnt = (uint16_t)((st >> 4U) & 0x7FFU);
        uint8_t pktsts = (uint8_t)((st >> 17U) & 0x0FU);
        uint16_t words = (uint16_t)((bcnt + 3U) / 4U);
        uint8_t tmp[64];
        uint16_t i;

        for (i = 0U; i < words; i++) {
            uint32_t w = OTG_FIFO(0U);
            if ((i * 4U) < sizeof(tmp)) {
                bl_memcpy(tmp + (i * 4U), &w, 4U);
            }
        }

        if (ep == 0U && pktsts == 6U) { /* SETUP packet */
            usb_handle_setup(tmp);
            usb_arm_ep_out(0U);
        } else if (ep == 0U && pktsts == 2U) {
            usb_arm_ep_out(0U);
        } else if (ep == 1U && pktsts == 2U && bcnt > 0U) { /* Bulk OUT data */
            if (g_usb_rx_offset + bcnt <= sizeof(g_rx_pkt_buf)) {
                bl_memcpy(g_rx_pkt_buf + g_usb_rx_offset, tmp, bcnt);
                g_usb_rx_offset = (uint16_t)(g_usb_rx_offset + bcnt);
            }
            if (g_usb_rx_offset >= sizeof(struct nlk_pkt_hdr)) {
                const struct nlk_pkt_hdr *h = (const struct nlk_pkt_hdr *)g_rx_pkt_buf;
                g_usb_rx_expected = (uint16_t)(sizeof(*h) + h->payload_len);
                if (g_usb_rx_expected > sizeof(g_rx_pkt_buf)) {
                    g_usb_rx_offset = 0U;
                    g_usb_rx_expected = 0U;
                } else if (g_usb_rx_offset >= g_usb_rx_expected) {
                    uint16_t resp_len = bl_handle_nlk_packet(
                        g_rx_pkt_buf, g_usb_rx_expected, g_tx_pkt_buf);
                    g_usb_rx_offset = 0U;
                    g_usb_rx_expected = 0U;
                    if (resp_len > 0U) {
                        usb_send_bulk_in(g_tx_pkt_buf, resp_len);
                    }
                }
            }
            usb_arm_ep_out(1U);
        }
    }
}

static void bootloader_service_loop(void)
{
    uint8_t dec_state = 0U;
    uint16_t exp_len = 0U;
    uint16_t rx_pos = 0U;
    uint16_t rx_crc = 0U;

    rgb_set_solid(0U, 0U, 255U);
    usb_otg_init();
    uart_puts("[BL] Entering NLK Bootloader Mode (Direct Internal Flash)\r\n");

    for (;;) {
        /* 轮询 USB OTG FS WinUSB 端点 */
        usb_otg_poll();

        /* 轮询 UART1 接收 L2 Stream 帧 */
        if ((USART1->SR & (1UL << 5U)) != 0UL) {
            uint8_t b = (uint8_t)USART1->DR;
            switch (dec_state) {
            case 0U:
                if (b == NLK_SOF_BYTE0) {
                    dec_state = 1U;
                }
                break;
            case 1U:
                dec_state = (b == NLK_SOF_BYTE1) ? 2U : ((b == NLK_SOF_BYTE0) ? 1U : 0U);
                break;
            case 2U:
                exp_len = b;
                dec_state = 3U;
                break;
            case 3U:
                exp_len |= (uint16_t)((uint16_t)b << 8U);
                dec_state = 4U;
                break;
            case 4U: {
                uint8_t lb[2] = { (uint8_t)(exp_len & 0xFFU), (uint8_t)(exp_len >> 8U) };
                if (b == bl_crc8(lb, 2U) && exp_len >= sizeof(struct nlk_pkt_hdr) &&
                    exp_len <= sizeof(g_rx_pkt_buf)) {
                    rx_pos = 0U;
                    dec_state = 5U;
                } else {
                    dec_state = 0U;
                }
                break;
            }
            case 5U:
                g_rx_pkt_buf[rx_pos++] = b;
                if (rx_pos >= exp_len) {
                    dec_state = 6U;
                }
                break;
            case 6U:
                rx_crc = b;
                dec_state = 7U;
                break;
            case 7U:
                rx_crc |= (uint16_t)((uint16_t)b << 8U);
                dec_state = 0U;
                if (rx_crc == bl_crc16(g_rx_pkt_buf, exp_len)) {
                    uint16_t resp_len = bl_handle_nlk_packet(g_rx_pkt_buf, exp_len, g_tx_pkt_buf);
                    if (resp_len > 0U) {
                        uart_send_nlk_frame(g_tx_pkt_buf, resp_len);
                    }
                }
                break;
            default:
                dec_state = 0U;
                break;
            }
        }

        if (g_bl_flash.reboot_pending) {
            delay_us(10000U);
            if (g_bl_flash.reboot_flag == BOOT_BOOTLOADER) {
                RTC_BKP0R = get_boot_flag_data(BOOT_BOOTLOADER);
            } else {
                RTC_BKP0R = 0UL;
            }
            SCB_AIRCR = 0x05FA0004UL;
            for (;;) {
            }
        }
    }
}

int main(void)
{
    uint32_t bkp_raw;
    bool enter_bl = false;

    clock_init_84mhz();
    uart1_init();
    rgb_init();

    uart_puts("\r\n[BL] NK60-V2 Direct-Flash Bootloader\r\n");

    bkp_raw = RTC_BKP0R;
    RTC_BKP0R = 0UL; /* 单次消费启动标志 */

    if (get_boot_flag_magic(bkp_raw) == BOOT_FLAG_MAGIC &&
        get_boot_flag(bkp_raw) == BOOT_BOOTLOADER) {
        uart_puts("[BL] Reboot-to-bootloader flag detected\r\n");
        enter_bl = true;
    } else if (key_scan_recovery_pressed()) {
        uart_puts("[BL] Physical recovery key combo detected\r\n");
        enter_bl = true;
    } else if (!is_internal_kernel_valid()) {
        uart_puts("[BL] No valid kernel at 0x08004000, staying in bootloader\r\n");
        enter_bl = true;
    }

    if (!enter_bl) {
        uart_puts("[BL] Booting NOS @ 0x08004000\r\n");
        jump_to_kernel(NK60_KERNEL_FLASH_BASE);
    }

    bootloader_service_loop();
    return 0;
}
