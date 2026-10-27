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

#include <kernel/cpu.h>
#include <kernel/device.h>
#include <kernel/errno.h>
#include <kernel/init.h>
#include <kernel/printk.h>
#include <kernel/sleep.h>
#include <kernel/timer.h>
#include <kernel/clk.h>
#include <string.h>

#include <nos/nlk_core.h>
#include <nos/nlk_caps.h>
#include <nos/nlk_bearer.h>
#include "board.h"

/* NK60-V2 W25QXX External Flash Partition Layout */
#define NK60_PART_BOOTLOADER        0U
#define NK60_PART_KERNEL_A          1U
#define NK60_PART_KERNEL_B          2U
#define NK60_PART_ASSETS            3U

#define NK60_BOOTLOADER_ADDR        0x00000UL
#define NK60_KERNEL_A_ADDR          0x08000UL
#define NK60_KERNEL_B_ADDR          0x3A000UL
#define NK60_ASSETS_ADDR            0x80000UL
#define NK60_REBOOT_FLASH_BASE      0x7F00000UL

static const uint32_t g_nk60_part_max_sizes[NLK_FW_MAX_PARTS] = {
    [NK60_PART_BOOTLOADER] = 32UL * 1024UL,
    [NK60_PART_KERNEL_A]   = 200UL * 1024UL,
    [NK60_PART_KERNEL_B]   = 200UL * 1024UL,
    [NK60_PART_ASSETS]     = 512UL * 1024UL,
};

struct nk60_nlk_board {
    struct device           *flash_dev;
    struct nlk_node         *node;
    struct timer             reboot_timer;
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

static int nk60_flash_write(uint8_t part_index, uint32_t offset,
                            const uint8_t *data, uint32_t len, void *priv)
{
    struct nk60_nlk_board *brd = (struct nk60_nlk_board *)priv;
    struct device *fdev;
    addr_t base_addr;

    if (brd == NULL) {
        return -EINVAL;
    }
    switch (part_index) {
    case NK60_PART_BOOTLOADER:
        base_addr = NK60_BOOTLOADER_ADDR;
        break;
    case NK60_PART_KERNEL_A:
        base_addr = NK60_KERNEL_A_ADDR;
        break;
    case NK60_PART_KERNEL_B:
        base_addr = NK60_KERNEL_B_ADDR;
        break;
    case NK60_PART_ASSETS:
        base_addr = NK60_ASSETS_ADDR;
        break;
    default:
        return -EINVAL;
    }

    fdev = nk60_get_flash_dev(brd);
    if (fdev != NULL && fdev->ops.write != NULL) {
        fdev->ops.write(fdev, base_addr + offset, (const void *)data, len);
    }
    return 0;
}

static int nk60_flash_commit(uint8_t part_index, uint32_t total_size,
                             uint32_t crc32, void *priv)
{
    struct nk60_nlk_board *brd = (struct nk60_nlk_board *)priv;
    struct device *fdev;
    uint32_t reboot_flag = REBOOT_TO_BOOTLOADER;

    if (brd == NULL) {
        return -EINVAL;
    }
    if (part_index == NK60_PART_KERNEL_A) {
        reboot_flag = REBOOT_TO_KERNEL_A;
    } else if (part_index == NK60_PART_KERNEL_B) {
        reboot_flag = REBOOT_TO_KERNEL_B;
    }

    fdev = nk60_get_flash_dev(brd);
    if (fdev != NULL && fdev->ops.write != NULL) {
        fdev->ops.write(fdev, NK60_REBOOT_FLASH_BASE,
                        &reboot_flag, sizeof(reboot_flag));
    }
    pr_info("NK60 firmware commit ok: part=%u, size=%u, crc=0x%08x\r\n",
            part_index, total_size, crc32);
    return 0;
}

static void nk60_reboot_timer_cb(void *parameter)
{
    (void)parameter;
    cpu_reboot(REBOOT_TO_BOOTLOADER);
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
    uint32_t ver_code = 0x00010000UL;
    int i;

#ifdef CONFIG_VERSION_CODE
    ver_code = CONFIG_VERSION_CODE;
#endif

    memset(brd, 0, sizeof(*brd));
    brd->node = node;
    timer_init(&brd->reboot_timer, "nlk_rst", nk60_reboot_timer_cb, brd);

#ifdef CONFIG_FLASH_DEV
    brd->flash_dev = device_find_by_name(CONFIG_FLASH_DEV);
#endif

    (void)nlk_node_set_identity(node, 0x4E4BU, 0x0060U,
                                NLK_DEV_CLASS_KEYBOARD, "NK60-V2");

    /* 1. NK60-V2 Firmware Update Capability with W25QXX Partition Table */
    (void)nlk_cap_fw_update_init(node, &brd->fw_cap, ver_code,
                                 nk60_flash_write, nk60_flash_commit, brd);
    brd->fw_cap.flash_reboot = nk60_flash_reboot;
    (void)nlk_cap_fw_update_set_partitions(&brd->fw_cap, 4U,
                                           NK60_PART_KERNEL_A,
                                           g_nk60_part_max_sizes);

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
