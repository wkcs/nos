/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Board-Specific NLK (NOS Link Kit) Configuration:
 * - Node Identity (NK60-V2 60% RGB Mechanical Keyboard)
 * - W25QXX SPI Flash Partition Layout & OTA Callbacks
 * - Expansion Capabilities (Encoder, Display, Wireless Bridge)
 * - Upstream WinUSB Bearer Attachment
 *
 * Note: The NLK_CAP_KEYBOARD capability and USB HID reporting are registered
 * and managed by the system keyboard service (app/server/keyboard/keyboard_server.c).
 */

#define pr_fmt(fmt) "[NK60-NLK]:%s[%d]:"fmt, __func__, __LINE__

#include <stddef.h>
#include <kernel/cpu.h>
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

static const uint32_t g_nk60_part_max_sizes[NLK_FW_MAX_PARTS] = {
    [NK60_PART_BOOTLOADER] = NK60_BOOTLOADER_MAX_BIN + NK60_IMG_HDR_SIZE,
    [NK60_PART_KERNEL]     = NK60_KERNEL_MAX_BIN,
    [NK60_PART_ASSETS]     = NK60_W25Q_ASSETS_MAX_SIZE,
};

struct nk60_nlk_board {
    struct device           *flash_dev;
    struct nlk_node         *node;
    struct timer             reboot_timer;
    uint32_t                 pending_reboot_flag;
    uint32_t                 erased_up_to;
    bool                     staged_hdr_valid;
    struct nlk_fw_img_hdr    staged_hdr;
    struct nlk_winusb_bearer winusb_bearer;
    struct nlk_fw_update_ctx fw_cap;
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

static int nk60_part_base_addr(uint8_t part_index, addr_t *base_addr)
{
    switch (part_index) {
    case NK60_PART_BOOTLOADER:
        *base_addr = NK60_W25Q_BOOTLOADER_ADDR;
        return 0;
    case NK60_PART_KERNEL:
        *base_addr = NK60_W25Q_KERNEL_ADDR;
        return 0;
    case NK60_PART_ASSETS:
        *base_addr = NK60_W25Q_ASSETS_ADDR;
        return 0;
    default:
        return -EINVAL;
    }
}

static int nk60_program_bootloader_sector0(struct nk60_nlk_board *brd,
                                           uint32_t payload_size,
                                           uint32_t expected_payload_crc)
{
    struct device *fdev = nk60_get_flash_dev(brd);
    uint8_t buf[256];
    uint32_t offset = 0UL;
    uint32_t calc_crc;
    addr_t irq_flags;
    int rc = 0;

    if (fdev == NULL || fdev->ops.read == NULL ||
        payload_size == 0UL || payload_size > NK60_BOOTLOADER_MAX_BIN) {
        return -EINVAL;
    }

    irq_flags = disable_irq_save();

    if ((FLASH->CR & FLASH_CR_LOCK) != 0UL) {
        FLASH->KEYR = STM32_FLASH_KEY1;
        FLASH->KEYR = STM32_FLASH_KEY2;
    }
    while ((FLASH->SR & FLASH_SR_BSY) != 0UL) {
    }
    FLASH->SR = STM32_FLASH_SR_ERR_MASK | FLASH_SR_EOP;

    /* Erase Sector 0 (0x08000000 - 0x08003FFF, 16 KB) */
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

    enable_irq_save(irq_flags);

    while (offset < payload_size) {
        uint32_t chunk = payload_size - offset;
        uint32_t i;

        if (chunk > sizeof(buf)) {
            chunk = sizeof(buf);
        }
        memset(buf, 0xFF, sizeof(buf));
        if (fdev->ops.read(fdev,
                           NK60_W25Q_BOOTLOADER_ADDR + NK60_IMG_HDR_SIZE + offset,
                           buf, chunk) != (ssize_t)chunk) {
            rc = -EIO;
            break;
        }

        irq_flags = disable_irq_save();
        for (i = 0UL; i < chunk; i += 4UL) {
            uint32_t word;
            uint32_t dst = NK60_BOOTLOADER_FLASH_BASE + offset + i;

            memcpy(&word, &buf[i], sizeof(word));
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
        enable_irq_save(irq_flags);
        if (rc != 0) {
            break;
        }
        offset += chunk;
    }

    irq_flags = disable_irq_save();
    /* Reset ART instruction/data caches so Sector 0 read-back sees new Flash data */
    FLASH->ACR &= ~(FLASH_ACR_ICEN | FLASH_ACR_DCEN);
    FLASH->ACR |= (FLASH_ACR_ICRST | FLASH_ACR_DCRST);
    FLASH->ACR &= ~(FLASH_ACR_ICRST | FLASH_ACR_DCRST);
    FLASH->ACR |= (FLASH_ACR_ICEN | FLASH_ACR_DCEN);
    FLASH->CR |= FLASH_CR_LOCK;
    enable_irq_save(irq_flags);

    if (rc != 0) {
        return rc;
    }

    calc_crc = nlk_crc32(0U, (const void *)(uintptr_t)NK60_BOOTLOADER_FLASH_BASE,
                         payload_size);
    if (expected_payload_crc != 0UL && calc_crc != expected_payload_crc) {
        pr_err("Sector 0 bootloader CRC mismatch: calc=0x%08x != exp=0x%08x\r\n",
               (unsigned int)calc_crc, (unsigned int)expected_payload_crc);
        return -EBADMSG;
    }
    return 0;
}

static int nk60_flash_prepare(uint8_t part_index, uint32_t total_size, void *priv)
{
    struct nk60_nlk_board *brd = (struct nk60_nlk_board *)priv;
    addr_t base_addr;

    if (brd == NULL || nk60_part_base_addr(part_index, &base_addr) < 0) {
        return -EINVAL;
    }
    brd->fw_cap.active_part = NK60_PART_KERNEL;
    if (part_index != NK60_PART_ASSETS && total_size <= NK60_IMG_HDR_SIZE) {
        return -EINVAL;
    }
    brd->erased_up_to = 0UL;
    brd->staged_hdr_valid = false;
    memset(&brd->staged_hdr, 0, sizeof(brd->staged_hdr));
    return 0;
}

static int nk60_flash_write(uint8_t part_index, uint32_t offset,
                            const uint8_t *data, uint32_t len, void *priv)
{
    struct nk60_nlk_board *brd = (struct nk60_nlk_board *)priv;
    struct device *fdev;
    addr_t base_addr;
    uint32_t end_off;

    if (brd == NULL || data == NULL ||
        nk60_part_base_addr(part_index, &base_addr) < 0) {
        return -EINVAL;
    }

    if (offset == 0UL) {
        brd->erased_up_to = 0UL;
        brd->staged_hdr_valid = false;

        if (part_index != NK60_PART_ASSETS) {
            const struct nlk_fw_img_hdr *hdr = (const struct nlk_fw_img_hdr *)data;
            uint8_t exp_type = (part_index == NK60_PART_BOOTLOADER) ?
                               NLK_FW_IMG_TYPE_BOOTLOADER : NLK_FW_IMG_TYPE_KERNEL;
            uint32_t max_bin = (part_index == NK60_PART_BOOTLOADER) ?
                               NK60_BOOTLOADER_MAX_BIN :
                               (NK60_KERNEL_MAX_BIN - NK60_IMG_HDR_SIZE);

            if (len < sizeof(*hdr) || hdr->magic != NLK_FW_IMG_MAGIC ||
                hdr->type != exp_type || hdr->size == 0UL || hdr->size > max_bin) {
                pr_err("Invalid firmware image header for part %u\r\n", part_index);
                return -EBADMSG;
            }
            if (len >= sizeof(*hdr) + 8UL) {
                uint32_t msp;
                uint32_t reset_pc;
                uint32_t flash_start = (part_index == NK60_PART_BOOTLOADER) ?
                                       NK60_BOOTLOADER_FLASH_BASE :
                                       NK60_KERNEL_FLASH_BASE;
                uint32_t flash_end = (part_index == NK60_PART_BOOTLOADER) ?
                                     (NK60_BOOTLOADER_FLASH_BASE + NK60_BOOTLOADER_MAX_BIN) :
                                     NK60_INT_FLASH_END;

                memcpy(&msp, data + sizeof(*hdr), sizeof(msp));
                memcpy(&reset_pc, data + sizeof(*hdr) + 4UL, sizeof(reset_pc));
                if (!nk60_validate_cortexm_vectors(msp, reset_pc, flash_start, flash_end)) {
                    pr_err("Invalid Cortex-M4 vectors (MSP=0x%08x, PC=0x%08x) for part %u\r\n",
                           (unsigned int)msp, (unsigned int)reset_pc, part_index);
                    return -EBADMSG;
                }
            }
            brd->staged_hdr = *hdr;
            brd->staged_hdr_valid = true;
        }
    }

    fdev = nk60_get_flash_dev(brd);
    if (fdev == NULL || fdev->ops.write == NULL) {
        return -ENODEV;
    }

    /* Pre-erase each 4 KB W25QXX sector once upon entry to avoid read-erase-modify amplification */
    end_off = offset + len;
    if (fdev->ops.control != NULL) {
        while (brd->erased_up_to < end_off) {
            uint32_t sec_addr = (uint32_t)base_addr + brd->erased_up_to;
            if (fdev->ops.control(fdev, W25Q_FLASH_CTRL_ERASE_SECTOR, &sec_addr) < 0) {
                return -EIO;
            }
            brd->erased_up_to += NK60_W25Q_SECTOR_SIZE;
        }
    }

    if (fdev->ops.write(fdev, base_addr + offset, (const void *)data, len) < 0) {
        return -EIO;
    }
    return 0;
}

static int nk60_flash_commit(uint8_t part_index, uint32_t total_size,
                             uint32_t crc32, void *priv)
{
    struct nk60_nlk_board *brd = (struct nk60_nlk_board *)priv;
    struct device *fdev;
    addr_t base_addr;
    uint8_t buf[256];
    uint32_t offset = 0UL;
    uint32_t readback_crc = 0UL;
    uint32_t payload_crc = 0UL;
    uint32_t ver_code = 0UL;
    bool full_image = false;
    int rc;

    if (brd == NULL || nk60_part_base_addr(part_index, &base_addr) < 0) {
        return -EINVAL;
    }
    fdev = nk60_get_flash_dev(brd);
    if (fdev == NULL || fdev->ops.read == NULL) {
        return -ENODEV;
    }

    /* Read back from W25QXX and verify full image CRC32 + payload CRC32 */
    while (offset < total_size) {
        uint32_t chunk = total_size - offset;

        if (chunk > sizeof(buf)) {
            chunk = sizeof(buf);
        }
        if (fdev->ops.read(fdev, base_addr + offset, buf, chunk) != (ssize_t)chunk) {
            return -EIO;
        }
        readback_crc = nlk_crc32(readback_crc, buf, chunk);
        if (part_index != NK60_PART_ASSETS && offset + chunk > NK60_IMG_HDR_SIZE) {
            uint32_t pay_off = (offset < NK60_IMG_HDR_SIZE) ?
                               (NK60_IMG_HDR_SIZE - offset) : 0UL;
            payload_crc = nlk_crc32(payload_crc, buf + pay_off, chunk - pay_off);
        }
        offset += chunk;
    }

    if (readback_crc != crc32) {
        pr_err("W25QXX read-back CRC mismatch: calc=0x%08x != exp=0x%08x\r\n",
               (unsigned int)readback_crc, (unsigned int)crc32);
        return -EBADMSG;
    }

    if (part_index != NK60_PART_ASSETS && brd->staged_hdr_valid) {
        full_image = (total_size == (NK60_IMG_HDR_SIZE + brd->staged_hdr.size));
        if (full_image && brd->staged_hdr.payload_crc32 != 0UL &&
            payload_crc != brd->staged_hdr.payload_crc32) {
            pr_err("Payload CRC mismatch: calc=0x%08x != hdr=0x%08x\r\n",
                   (unsigned int)payload_crc,
                   (unsigned int)brd->staged_hdr.payload_crc32);
            return -EBADMSG;
        }
    }
    (void)ver_code;

    if (part_index == NK60_PART_BOOTLOADER) {
        if (!full_image) {
            return -EINVAL;
        }
        (void)board_write_boot_flag(NK60_BOOT_CMD_UPDATE_BL);

        rc = nk60_program_bootloader_sector0(brd, brd->staged_hdr.size, payload_crc);
        if (rc < 0) {
            return rc;
        }

        (void)board_write_boot_flag(REBOOT_TO_KERNEL);
        brd->pending_reboot_flag = REBOOT_TO_KERNEL;
    } else if (part_index == NK60_PART_KERNEL) {
        if (full_image) {
            brd->pending_reboot_flag = REBOOT_TO_KERNEL_A;
            (void)board_write_boot_flag(brd->pending_reboot_flag);
        }
    } else if (part_index == NK60_PART_ASSETS) {
        (void)nk60_assets_reload();
    }

    pr_info("NK60 firmware commit ok: part=%u, size=%u, crc=0x%08x\r\n",
            part_index, total_size, crc32);
    return 0;
}

static void nk60_reboot_timer_cb(void *parameter)
{
    struct nk60_nlk_board *brd = (struct nk60_nlk_board *)parameter;
    uint32_t flag = (brd != NULL) ? brd->pending_reboot_flag : REBOOT_TO_BOOTLOADER;

    cpu_reboot(flag);
}

static void nk60_flash_reboot(uint8_t part_index, void *priv)
{
    struct nk60_nlk_board *brd = (struct nk60_nlk_board *)priv;

    (void)part_index;
    if (brd == NULL) {
        return;
    }
    pr_info("NK60 scheduling reboot after NLK ACK...\r\n");
    timer_start(&brd->reboot_timer, msec_to_tick(50));
}

static int nk60_nlk_board_setup(struct nlk_node *node)
{
    struct nk60_nlk_board *brd = &g_nk60_nlk;
    uint32_t part_sizes[NLK_FW_MAX_PARTS];
    struct w25q_wl *assets_wl;
    uint32_t ver_code = 0x00010000UL;
    int i;

#ifdef CONFIG_VERSION_CODE
    ver_code = CONFIG_VERSION_CODE;
#endif

    memset(brd, 0, sizeof(*brd));
    brd->node = node;
    brd->pending_reboot_flag = REBOOT_TO_BOOTLOADER;
    timer_init(&brd->reboot_timer, "nlk_rst", nk60_reboot_timer_cb, brd);

#ifdef CONFIG_FLASH_DEV
    brd->flash_dev = device_find_by_name(CONFIG_FLASH_DEV);
#endif

    (void)nlk_node_set_identity(node, 0x4E4BU, 0x0060U,
                                NLK_DEV_CLASS_KEYBOARD, "NK60-V2");

    /* 1. NK60-V2 Firmware Update Capability with W25QXX Partition Table */
    (void)nlk_cap_fw_update_init(node, &brd->fw_cap, ver_code,
                                 nk60_flash_write, nk60_flash_commit, brd);
    brd->fw_cap.flash_prepare = nk60_flash_prepare;
    brd->fw_cap.flash_reboot = nk60_flash_reboot;
    memcpy(part_sizes, g_nk60_part_max_sizes, sizeof(part_sizes));
    assets_wl = nk60_flash_get_assets_wl();
    if (assets_wl != NULL) {
        part_sizes[NK60_PART_ASSETS] =
            (uint32_t)assets_wl->logical_blocks * NK60_W25Q_SECTOR_SIZE;
    }
    (void)nlk_cap_fw_update_set_partitions(&brd->fw_cap, NK60_PART_COUNT,
                                           NK60_PART_KERNEL,
                                           part_sizes);

    /* 2. Optional Expansion Capabilities (Encoder, Display, Wireless Bridge) */
    (void)nlk_cap_encoder_init(node, &brd->enc_cap, 24U, false, NULL, brd);
    (void)nlk_cap_display_init(node, &brd->disp_cap, 128U, 64U,
                               NLK_PIXEL_FMT_MONO_1BPP, NULL, brd);
    (void)nlk_cap_wireless_init(node, &brd->wl_cap,
                                (1U << NLK_WL_CHAN_USB_WIRED) |
                                (1U << NLK_WL_CHAN_BLE_SLOT1) |
                                (1U << NLK_WL_CHAN_BLE_SLOT2) |
                                (1U << NLK_WL_CHAN_BLE_SLOT3) |
                                (1U << NLK_WL_CHAN_RF_24G), 4U);

    /* 3. Attach Upstream WinUSB Bearer */
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
