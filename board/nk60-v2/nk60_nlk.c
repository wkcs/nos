/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 板级 NLK (NOS Link Kit) 配置：
 * - 节点标识 (NK60-V2 60% RGB 机械键盘)
 * - NLK_CAP_FLASH 通用分区注册：
 *     1. "bootloader" -> 直接擦写 STM32F401 内部 Flash Sector 0 (0x08000000, 16 KB)
 *     2. "kernel"     -> STM32F401 内部 Flash Sector 1~5 (0x08004000, 240 KB, 需进入 Bootloader 直刷)
 *     3. "assets"     -> 外部 W25QXX SPI Flash (0x048000, 736 KB FatFs 资源区)
 * - 扩展能力注册 (Encoder, Display, Wireless Bridge)
 * - 上行 WinUSB Bearer 挂载
 */

#define pr_fmt(fmt) "[NK60-NLK]:%s[%d]:"fmt, __func__, __LINE__

#include <stddef.h>
#include <kernel/cpu.h>
#include <kernel/boot.h>
#include <kernel/device.h>
#include <kernel/errno.h>
#include <kernel/init.h>
#include <kernel/irq.h>
#include <kernel/printk.h>
#include <kernel/sleep.h>
#include <kernel/timer.h>
#include <kernel/clk.h>
#include <string.h>

#include <drivers/w25q_flash.h>
#include <nos/nlk_core.h>
#include <nos/nlk_caps.h>
#include <nos/nlk_bearer.h>
#include <board/boot.h>
#include "board.h"

#define STM32_FLASH_KEY1            0x45670123UL
#define STM32_FLASH_KEY2            0xCDEF89ABUL
#define STM32_FLASH_SR_ERR_MASK     0x000000F2UL
#define STM32_FLASH_PSIZE_WORD      FLASH_CR_PSIZE_1

struct nk60_nlk_board {
    struct device           *flash_dev;
    struct nlk_node         *node;
    struct timer             reboot_timer;
    uint32_t                 pending_reboot_flag;
    uint32_t                 erased_up_to;
    struct nlk_winusb_bearer winusb_bearer;
    struct nlk_flash_ctx     flash_cap;
    struct nlk_encoder_ctx   enc_cap;
    struct nlk_display_ctx   disp_cap;
    struct nlk_wireless_ctx  wl_cap;
};

static struct nk60_nlk_board g_nk60_nlk;

static struct device *nk60_get_flash_dev(struct nk60_nlk_board *brd)
{
#ifdef CONFIG_FLASH_DEV
    if (brd->flash_dev == NULL) {
        brd->flash_dev = device_find_by_name(CONFIG_FLASH_DEV);
    }
#endif
    return brd->flash_dev;
}

/* =========================================================================
 * 分区 0 ("bootloader")：直接擦写 STM32F401 内部 Flash Sector 0 (0x08000000)
 * ========================================================================= */
static int nk60_bl_prepare(uint32_t total_size, uint32_t expected_crc32, void *priv)
{
    addr_t irq_flags;

    (void)expected_crc32;
    (void)priv;
    if (total_size == 0UL || total_size > NK60_BOOTLOADER_MAX_BIN) {
        return -EFBIG;
    }

    irq_flags = disable_irq_save();
    if ((FLASH->CR & FLASH_CR_LOCK) != 0UL) {
        FLASH->KEYR = STM32_FLASH_KEY1;
        FLASH->KEYR = STM32_FLASH_KEY2;
    }
    while ((FLASH->SR & FLASH_SR_BSY) != 0UL) {
    }
    FLASH->SR = STM32_FLASH_SR_ERR_MASK | FLASH_SR_EOP;

    /* 擦除 Sector 0 (0x08000000 - 0x08003FFF, 16 KB) */
    FLASH->CR = STM32_FLASH_PSIZE_WORD | FLASH_CR_SER | (0UL << 3U);
    FLASH->CR |= FLASH_CR_STRT;
    __DSB();
    while ((FLASH->SR & FLASH_SR_BSY) != 0UL) {
    }
    FLASH->CR &= ~(FLASH_CR_SER | FLASH_CR_SNB);
    if ((FLASH->SR & STM32_FLASH_SR_ERR_MASK) != 0UL) {
        FLASH->CR |= FLASH_CR_LOCK;
        enable_irq_save(irq_flags);
        return -EIO;
    }
    FLASH->CR |= FLASH_CR_LOCK;
    enable_irq_save(irq_flags);
    return 0;
}

static int nk60_bl_write(uint32_t offset, const uint8_t *data,
                         uint32_t len, void *priv)
{
    uint32_t i;
    addr_t irq_flags;
    int rc = 0;

    (void)priv;
    if (data == NULL || offset + len > NK60_BOOTLOADER_MAX_BIN) {
        return -EINVAL;
    }

    /* 首包校验 Cortex-M4 向量表合法性（栈顶 MSP 与复位向量 Reset_Handler） */
    if (offset == 0UL && len >= 8UL) {
        uint32_t msp;
        uint32_t reset_pc;

        memcpy(&msp, data, sizeof(msp));
        memcpy(&reset_pc, data + 4UL, sizeof(reset_pc));
        if (!nk60_validate_cortexm_vectors(
                msp, reset_pc,
                NK60_BOOTLOADER_FLASH_BASE,
                NK60_BOOTLOADER_FLASH_BASE + NK60_BOOTLOADER_MAX_BIN)) {
            pr_err("Invalid Bootloader vectors (MSP=0x%08x, PC=0x%08x)\r\n",
                   (unsigned int)msp, (unsigned int)reset_pc);
            return -EBADMSG;
        }
    }

    irq_flags = disable_irq_save();
    if ((FLASH->CR & FLASH_CR_LOCK) != 0UL) {
        FLASH->KEYR = STM32_FLASH_KEY1;
        FLASH->KEYR = STM32_FLASH_KEY2;
    }

    for (i = 0UL; i < len; i += 4UL) {
        uint32_t word = 0xFFFFFFFFUL;
        uint32_t rem = len - i;
        uint32_t dst = NK60_BOOTLOADER_FLASH_BASE + offset + i;

        memcpy(&word, data + i, (rem >= 4UL) ? 4U : rem);
        while ((FLASH->SR & FLASH_SR_BSY) != 0UL) {
        }
        FLASH->SR = STM32_FLASH_SR_ERR_MASK | FLASH_SR_EOP;
        FLASH->CR = STM32_FLASH_PSIZE_WORD | FLASH_CR_PG;
        *(volatile uint32_t *)(uintptr_t)dst = word;
        __DSB();
        while ((FLASH->SR & FLASH_SR_BSY) != 0UL) {
        }
        FLASH->CR &= ~FLASH_CR_PG;
        if ((FLASH->SR & STM32_FLASH_SR_ERR_MASK) != 0UL) {
            rc = -EIO;
            break;
        }
    }

    FLASH->CR |= FLASH_CR_LOCK;
    enable_irq_save(irq_flags);
    return rc;
}

static int nk60_bl_commit(uint32_t total_size, uint32_t expected_crc32, void *priv)
{
    addr_t irq_flags;
    uint32_t calc_crc;

    (void)priv;
    irq_flags = disable_irq_save();
    /* 刷新 STM32 ART 指令与数据缓存，确保读回最新写入的内部 Flash 数据 */
    FLASH->ACR &= ~(FLASH_ACR_ICEN | FLASH_ACR_DCEN);
    FLASH->ACR |= (FLASH_ACR_ICRST | FLASH_ACR_DCRST);
    FLASH->ACR &= ~(FLASH_ACR_ICRST | FLASH_ACR_DCRST);
    FLASH->ACR |= (FLASH_ACR_ICEN | FLASH_ACR_DCEN);
    enable_irq_save(irq_flags);

    calc_crc = nlk_crc32(0U, (const void *)(uintptr_t)NK60_BOOTLOADER_FLASH_BASE,
                         total_size);
    if (expected_crc32 != 0UL && calc_crc != expected_crc32) {
        pr_err("Sector 0 bootloader CRC mismatch: calc=0x%08x != exp=0x%08x\r\n",
               (unsigned int)calc_crc, (unsigned int)expected_crc32);
        return -EBADMSG;
    }

    pr_info("NK60 Sector 0 bootloader flashed directly OK (size=%u, crc=0x%08x)\r\n",
            (unsigned int)total_size, (unsigned int)calc_crc);
    return 0;
}

/* =========================================================================
 * 分区 2 ("assets")：擦写外部 W25QXX SPI Flash 资源区 (0x048000)
 * ========================================================================= */
static int nk60_assets_prepare(uint32_t total_size, uint32_t expected_crc32, void *priv)
{
    struct nk60_nlk_board *brd = (struct nk60_nlk_board *)priv;

    (void)expected_crc32;
    if (brd == NULL || total_size == 0UL || total_size > NK60_W25Q_ASSETS_MAX_SIZE) {
        return -EINVAL;
    }
    brd->erased_up_to = 0UL;
    return 0;
}

static int nk60_assets_write(uint32_t offset, const uint8_t *data,
                             uint32_t len, void *priv)
{
    struct nk60_nlk_board *brd = (struct nk60_nlk_board *)priv;
    struct device *fdev;
    uint32_t end_off;

    if (brd == NULL || data == NULL ||
        offset + len > NK60_W25Q_ASSETS_MAX_SIZE) {
        return -EINVAL;
    }

    fdev = nk60_get_flash_dev(brd);
    if (fdev == NULL || fdev->ops.write == NULL) {
        return -ENODEV;
    }

    end_off = offset + len;
    if (fdev->ops.control != NULL) {
        while (brd->erased_up_to < end_off) {
            uint32_t sec_addr = NK60_W25Q_ASSETS_ADDR + brd->erased_up_to;
            if (fdev->ops.control(fdev, W25Q_FLASH_CTRL_ERASE_SECTOR, &sec_addr) < 0) {
                return -EIO;
            }
            brd->erased_up_to += NK60_W25Q_SECTOR_SIZE;
        }
    }

    if (fdev->ops.write(fdev, NK60_W25Q_ASSETS_ADDR + offset,
                        (const void *)data, len) < 0) {
        return -EIO;
    }
    return 0;
}

static int nk60_assets_commit(uint32_t total_size, uint32_t expected_crc32, void *priv)
{
    struct nk60_nlk_board *brd = (struct nk60_nlk_board *)priv;
    struct device *fdev;
    uint8_t buf[256];
    uint32_t offset = 0UL;
    uint32_t readback_crc = 0UL;

    if (brd == NULL) {
        return -EINVAL;
    }
    fdev = nk60_get_flash_dev(brd);
    if (fdev == NULL || fdev->ops.read == NULL) {
        return -ENODEV;
    }

    while (offset < total_size) {
        uint32_t chunk = total_size - offset;
        if (chunk > sizeof(buf)) {
            chunk = sizeof(buf);
        }
        if (fdev->ops.read(fdev, NK60_W25Q_ASSETS_ADDR + offset,
                           buf, chunk) != (ssize_t)chunk) {
            return -EIO;
        }
        readback_crc = nlk_crc32(readback_crc, buf, chunk);
        offset += chunk;
    }

    if (expected_crc32 != 0UL && readback_crc != expected_crc32) {
        pr_err("W25QXX assets CRC mismatch: calc=0x%08x != exp=0x%08x\r\n",
               (unsigned int)readback_crc, (unsigned int)expected_crc32);
        return -EBADMSG;
    }

    (void)nk60_assets_reload();
    pr_info("NK60 assets partition commit OK (size=%u, crc=0x%08x)\r\n",
            (unsigned int)total_size, (unsigned int)readback_crc);
    return 0;
}

static void nk60_reboot_timer_cb(void *parameter)
{
    struct nk60_nlk_board *brd = (struct nk60_nlk_board *)parameter;
    uint32_t flag = (brd != NULL) ? brd->pending_reboot_flag : BOOT_NORMAL;

    cpu_reboot(flag);
}

static void nk60_on_reboot(uint32_t boot_flag, void *priv)
{
    struct nk60_nlk_board *brd = (struct nk60_nlk_board *)priv;

    if (brd == NULL) {
        return;
    }
    brd->pending_reboot_flag = boot_flag;
    pr_info("NK60 scheduling reboot (boot_flag=%u) after NLK ACK...\r\n",
            (unsigned int)boot_flag);
    timer_start(&brd->reboot_timer, msec_to_tick(50));
}

static int nk60_nlk_board_setup(struct nlk_node *node)
{
    struct nk60_nlk_board *brd = &g_nk60_nlk;
    struct w25q_wl *assets_wl;
    uint32_t assets_max = NK60_W25Q_ASSETS_MAX_SIZE;
    uint32_t ver_code = 0x00010000UL;
    int i;

#ifdef CONFIG_VERSION_CODE
    ver_code = CONFIG_VERSION_CODE;
#endif

    memset(brd, 0, sizeof(*brd));
    brd->node = node;
    brd->pending_reboot_flag = BOOT_NORMAL;
    timer_init(&brd->reboot_timer, "nlk_rst", nk60_reboot_timer_cb, brd);

#ifdef CONFIG_FLASH_DEV
    brd->flash_dev = device_find_by_name(CONFIG_FLASH_DEV);
#endif

    (void)nlk_node_set_identity(node, 0x4E4BU, 0x0060U,
                                NLK_DEV_CLASS_KEYBOARD, "NK60-V2");
    nlk_node_set_reboot_handler(node, nk60_on_reboot, brd);

    /* 1. 初始化 NLK_CAP_FLASH 并注册板级分区表 */
    (void)nlk_cap_flash_init(node, &brd->flash_cap, BOOT_NORMAL, ver_code);

    assets_wl = nk60_flash_get_assets_wl();
    if (assets_wl != NULL) {
        assets_max = (uint32_t)assets_wl->logical_blocks * NK60_W25Q_SECTOR_SIZE;
    }

    {
        const struct nlk_flash_part_ops bl_part = {
            .name       = "bootloader",
            .max_size   = NK60_BOOTLOADER_MAX_BIN,
            .erase_size = NK60_BOOTLOADER_MAX_BIN,
            .flags      = NLK_FLASH_PART_F_WRITABLE,
            .priv       = brd,
            .prepare    = nk60_bl_prepare,
            .write      = nk60_bl_write,
            .commit     = nk60_bl_commit,
        };
        const struct nlk_flash_part_ops kernel_part = {
            .name       = "kernel",
            .max_size   = NK60_KERNEL_MAX_BIN,
            .erase_size = 16UL * 1024UL,
            .flags      = NLK_FLASH_PART_F_WRITABLE |
                          NLK_FLASH_PART_F_NEEDS_BL |
                          NLK_FLASH_PART_F_REBOOT_AFTER,
            .priv       = brd,
            .prepare    = NULL,
            .write      = NULL,
            .commit     = NULL,
        };
        const struct nlk_flash_part_ops assets_part = {
            .name       = "assets",
            .max_size   = assets_max,
            .erase_size = NK60_W25Q_SECTOR_SIZE,
            .flags      = NLK_FLASH_PART_F_WRITABLE,
            .priv       = brd,
            .prepare    = nk60_assets_prepare,
            .write      = nk60_assets_write,
            .commit     = nk60_assets_commit,
        };

        (void)nlk_cap_flash_register_part(&brd->flash_cap, &bl_part);
        (void)nlk_cap_flash_register_part(&brd->flash_cap, &kernel_part);
        (void)nlk_cap_flash_register_part(&brd->flash_cap, &assets_part);
    }

    /* 2. 注册可选外设扩展能力 (Encoder, Display, Wireless Bridge) */
    (void)nlk_cap_encoder_init(node, &brd->enc_cap, 24U, false, NULL, brd);
    (void)nlk_cap_display_init(node, &brd->disp_cap, 128U, 64U,
                               NLK_PIXEL_FMT_MONO_1BPP, NULL, brd);
    (void)nlk_cap_wireless_init(node, &brd->wl_cap,
                                (1U << NLK_WL_CHAN_USB_WIRED) |
                                (1U << NLK_WL_CHAN_BLE_SLOT1) |
                                (1U << NLK_WL_CHAN_BLE_SLOT2) |
                                (1U << NLK_WL_CHAN_BLE_SLOT3) |
                                (1U << NLK_WL_CHAN_RF_24G), 4U);

    /* 3. 挂载上行 WinUSB Bearer */
    for (i = 0; i < 20; i++) {
        if (nlk_winusb_bearer_init(node, &brd->winusb_bearer, 0U) == 0) {
            pr_info("NK60 WinUSB bearer attached on port 0\r\n");
            break;
        }
        msleep(50);
    }

    return 0;
}

static int nk60_nlk_register(void)
{
    nlk_server_register_board_setup(nk60_nlk_board_setup);
    return 0;
}
SYS_INIT(nk60_nlk_register, INIT_LEVEL_APPLICATION);
