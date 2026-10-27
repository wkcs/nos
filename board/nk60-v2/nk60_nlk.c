/**
 * Copyright (C) 2024-2026 Nick Hu <huqihan@live.com>
 *
 * NK60-V2 Board-Specific NLK (NOS Link Kit) Configuration:
 * - Node Identity (NK60-V2 60% RGB Mechanical Keyboard)
 * - W25QXX SPI Flash Partition Layout & OTA Callbacks
 * - 60% 61-Key RGB Matrix Keyboard + USB HID Bridge
 * - Upstream WinUSB Bearer Attachment
 */

#define pr_fmt(fmt) "[NK60-NLK]:%s[%d]:"fmt, __func__, __LINE__

#include <kernel/cpu.h>
#include <kernel/device.h>
#include <kernel/errno.h>
#include <kernel/init.h>
#include <kernel/printk.h>
#include <kernel/sleep.h>
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
    struct device           *hid_dev;
    struct nlk_node         *node;
    struct nlk_winusb_bearer winusb_bearer;
    struct nlk_fw_update_ctx fw_cap;
    struct nlk_keyboard_ctx  kb_cap;
    struct nlk_encoder_ctx   enc_cap;
    struct nlk_display_ctx   disp_cap;
    struct nlk_wireless_ctx  wl_cap;
};

static struct nk60_nlk_board g_nk60_nlk;

static int nk60_flash_write(uint8_t part_index, uint32_t offset,
                            const uint8_t *data, uint32_t len, void *priv)
{
    struct nk60_nlk_board *brd = (struct nk60_nlk_board *)priv;
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

    if (brd->flash_dev != NULL && brd->flash_dev->ops.write != NULL) {
        brd->flash_dev->ops.write(brd->flash_dev, base_addr + offset,
                                  (const void *)data, len);
    }
    return 0;
}

static int nk60_flash_commit(uint8_t part_index, uint32_t total_size,
                             uint32_t crc32, void *priv)
{
    struct nk60_nlk_board *brd = (struct nk60_nlk_board *)priv;
    uint32_t reboot_flag = REBOOT_TO_BOOTLOADER;

    if (brd == NULL) {
        return -EINVAL;
    }
    if (part_index == NK60_PART_KERNEL_A) {
        reboot_flag = REBOOT_TO_KERNEL_A;
    } else if (part_index == NK60_PART_KERNEL_B) {
        reboot_flag = REBOOT_TO_KERNEL_B;
    }

    if (brd->flash_dev != NULL && brd->flash_dev->ops.write != NULL) {
        brd->flash_dev->ops.write(brd->flash_dev, NK60_REBOOT_FLASH_BASE,
                                  &reboot_flag, sizeof(reboot_flag));
    }
    pr_info("NK60 firmware commit ok: part=%u, size=%u, crc=0x%08x\r\n",
            part_index, total_size, crc32);
    return 0;
}

static void nk60_hid_report_cb(const struct nlk_kb_hid_report *report, void *priv)
{
    struct nk60_nlk_board *brd = (struct nk60_nlk_board *)priv;
    uint8_t usb_hid[8];

    if (brd == NULL || report == NULL) {
        return;
    }
    if (brd->hid_dev == NULL) {
        brd->hid_dev = device_find_by_name("hidd");
    }
    if (brd->hid_dev != NULL && brd->hid_dev->ops.write != NULL) {
        usb_hid[0] = report->modifiers;
        usb_hid[1] = report->reserved;
        memcpy(&usb_hid[2], report->keys, 6U);
        (void)brd->hid_dev->ops.write(brd->hid_dev, 1, usb_hid, sizeof(usb_hid));
    }
    if (brd->node != NULL && brd->kb_cap.cap.cap_id != 0U) {
        (void)nlk_node_send_event(brd->node, NLK_ADDR_HOST,
                                  brd->kb_cap.cap.cap_id,
                                  NLK_KB_EVT_HID_REPORT,
                                  report, sizeof(*report), false);
    }
}

static int nk60_nlk_board_setup(struct nlk_node *node)
{
    struct nk60_nlk_board *brd = &g_nk60_nlk;
    struct nlk_kb_desc kb_desc;
    uint32_t ver_code = 0x00010000UL;
    int i;

#ifdef CONFIG_VERSION_CODE
    ver_code = CONFIG_VERSION_CODE;
#endif

    memset(brd, 0, sizeof(*brd));
    brd->node = node;

#ifdef CONFIG_FLASH_DEV
    brd->flash_dev = device_find_by_name(CONFIG_FLASH_DEV);
#endif

    (void)nlk_node_set_identity(node, 0x4E4BU, 0x0060U,
                                NLK_DEV_CLASS_KEYBOARD, "NK60-V2");

    /* 1. NK60-V2 Firmware Update Capability with W25QXX Partition Table */
    (void)nlk_cap_fw_update_init(node, &brd->fw_cap, ver_code,
                                 nk60_flash_write, nk60_flash_commit, brd);
    (void)nlk_cap_fw_update_set_partitions(&brd->fw_cap, 4U,
                                           NK60_PART_KERNEL_A,
                                           g_nk60_part_max_sizes);

    /* 2. NK60-V2 60% 61-Key RGB Mechanical Keyboard Capability (with Integrated Lighting) */
    (void)nlk_cap_keyboard_init(node, &brd->kb_cap, 5U, 14U, 61U,
                                nk60_hid_report_cb, brd);
    kb_desc = brd->kb_cap.desc;
    kb_desc.form_factor = NLK_KB_FORM_60;
    kb_desc.switch_type = NLK_KB_SW_MECH;
    kb_desc.kb_flags = NLK_KB_FLAG_6KRO | NLK_KB_FLAG_NKRO |
                       NLK_KB_FLAG_MEDIA_KEYS | NLK_KB_FLAG_MODULAR_HUB |
                       NLK_KB_FLAG_LIGHT_RGB_KEY | NLK_KB_FLAG_LIGHT_UNDERGLOW |
                       NLK_KB_FLAG_LIGHT_INDICATORS;
    kb_desc.led_matrix_rows = 5U;
    kb_desc.led_matrix_cols = 14U;
    kb_desc.underglow_leds = 9U;
    kb_desc.led_count = 70U;
    (void)nlk_cap_keyboard_configure(&brd->kb_cap, &kb_desc);

    /* 3. Optional Expansion Capabilities (Encoder, Display, Wireless Bridge) */
    (void)nlk_cap_encoder_init(node, &brd->enc_cap, 24U, false, NULL, brd);
    (void)nlk_cap_display_init(node, &brd->disp_cap, 128U, 64U,
                               NLK_PIXEL_FMT_MONO_1BPP, NULL, brd);
    (void)nlk_cap_wireless_init(node, &brd->wl_cap,
                                (1U << NLK_WL_CHAN_USB_WIRED) |
                                (1U << NLK_WL_CHAN_BLE_SLOT1) |
                                (1U << NLK_WL_CHAN_BLE_SLOT2) |
                                (1U << NLK_WL_CHAN_BLE_SLOT3) |
                                (1U << NLK_WL_CHAN_RF_24G), 4U);

    /* 4. Attach Upstream WinUSB Bearer */
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
