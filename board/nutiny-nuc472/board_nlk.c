/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NuTiny-NUC472 板级 NLK (NOS Link Kit) 配置与分区管理：
 * - 节点标识: VID 0x0416, PID 0x5011, "nutiny-nuc472"
 * - NLK_CAP_FLASH 分区:
 *     1. "kernel" -> 只读属性 (flags=0, 384 KiB)
 *     2. "data"   -> 可读写擦除 (NLK_FLASH_PART_F_WRITABLE, 128 KiB, nuc472-lfs 块设备)
 * - 上行 WinUSB Bearer 挂载
 */

#define pr_fmt(fmt) "[NUC472-NLK]:%s[%d]:"fmt, __func__, __LINE__

#include <stddef.h>
#include <kernel/kernel.h>
#include <kernel/cpu.h>
#include <kernel/boot.h>
#include <kernel/device.h>
#include <kernel/errno.h>
#include <kernel/init.h>
#include <kernel/printk.h>
#include <kernel/sleep.h>
#include <kernel/timer.h>
#include <kernel/clk.h>
#include <drivers/block.h>
#include <string.h>

#include <nos/nlk_core.h>
#include <nos/nlk_caps.h>
#include <nos/nlk_bearer.h>

#define NUC472_KERNEL_MAX_SIZE  (384UL * 1024UL)
#define NUC472_DATA_MAX_SIZE    (128UL * 1024UL)
#define NUC472_SECTOR_SIZE      512UL
#define NUC472_PAGE_SIZE        2048UL

extern int board_write_boot_flag(uint32_t flag);

struct nutiny_nlk_board {
    const struct device      *flash_dev;
    struct nlk_node          *node;
    struct timer              reboot_timer;
    uint32_t                  pending_boot_flag;
    struct nlk_winusb_bearer  winusb_bearer;
    struct nlk_flash_ctx      flash_cap;
};

static struct nutiny_nlk_board g_nutiny_nlk;

static const struct device *nutiny_get_flash_dev(void)
{
    if (g_nutiny_nlk.flash_dev == NULL) {
        g_nutiny_nlk.flash_dev = device_find_by_name("nuc472-lfs");
    }
    return g_nutiny_nlk.flash_dev;
}

/* =========================================================================
 * 分区 1 ("data")：通过 "nuc472-lfs" 块驱动擦写 128 KiB LittleFS 区域
 * ========================================================================= */
static int nutiny_data_prepare(uint32_t total_size, uint32_t expected_crc32, void *priv)
{
    const struct device *dev = nutiny_get_flash_dev();
    const struct block_driver_api *api;
    uint32_t pages_to_erase;

    (void)expected_crc32;
    (void)priv;

    if (total_size > NUC472_DATA_MAX_SIZE) {
        return -EFBIG;
    }
    if (dev == NULL || dev->api == NULL) {
        return -ENODEV;
    }

    api = dev->api;
    if (api->erase == NULL) {
        return -ENOSYS;
    }

    /* 按 2048 字节页擦除整个待写入区域 (4 扇区一页) */
    pages_to_erase = (total_size + NUC472_PAGE_SIZE - 1UL) / NUC472_PAGE_SIZE;
    if (pages_to_erase == 0UL) {
        pages_to_erase = 1UL;
    }
    return api->erase(dev, 0UL, pages_to_erase * (NUC472_PAGE_SIZE / NUC472_SECTOR_SIZE));
}

static int nutiny_data_write(uint32_t offset, const uint8_t *buf, uint32_t len, void *priv)
{
    const struct device *dev = nutiny_get_flash_dev();
    const struct block_driver_api *api;
    uint32_t lba;
    uint32_t sectors;

    (void)priv;

    if (dev == NULL || dev->api == NULL) {
        return -ENODEV;
    }
    if ((offset % NUC472_SECTOR_SIZE) != 0UL || (len % NUC472_SECTOR_SIZE) != 0UL) {
        return -EINVAL;
    }
    if (offset + len > NUC472_DATA_MAX_SIZE) {
        return -EFBIG;
    }

    api = dev->api;
    if (api->write == NULL) {
        return -ENOSYS;
    }

    lba = offset / NUC472_SECTOR_SIZE;
    sectors = len / NUC472_SECTOR_SIZE;
    return api->write(dev, lba, buf, sectors);
}

static int nutiny_data_commit(uint32_t total_size, uint32_t crc32, void *priv)
{
    (void)total_size;
    (void)crc32;
    (void)priv;
    return 0;
}

static void nutiny_reboot_timer_cb(void *parameter)
{
    struct nutiny_nlk_board *brd = parameter;
    uint32_t flag = (brd != NULL) ? brd->pending_boot_flag : (uint32_t)BOOT_NORMAL;

    (void)board_write_boot_flag(flag);
    cpu_reboot(flag);
}

static void nutiny_on_reboot(uint32_t boot_flag, void *priv)
{
    struct nutiny_nlk_board *brd = priv;
    if (brd == NULL) {
        return;
    }
    brd->pending_boot_flag = boot_flag;
    pr_info("NuTiny scheduling reboot (flag=0x%08x)...\n", (unsigned int)boot_flag);
    timer_start(&brd->reboot_timer, msec_to_tick(50));
}

static int nutiny_nlk_setup(struct nlk_node *node)
{
    struct nutiny_nlk_board *brd = &g_nutiny_nlk;
    uint32_t ver_code = 0x00010000UL;
    int i;

    memset(brd, 0, sizeof(*brd));
    brd->node = node;
    brd->pending_boot_flag = (uint32_t)BOOT_NORMAL;
    timer_init(&brd->reboot_timer, "nlk_rst", nutiny_reboot_timer_cb, brd);

    (void)nlk_node_set_identity(node, 0x0416U, 0x5011U,
                                NLK_DEV_CLASS_GENERIC, "nutiny-nuc472");
    nlk_node_set_reboot_handler(node, nutiny_on_reboot, brd);

    /* 初始化 NLK_CAP_FLASH 并注册板级分区表 */
    (void)nlk_cap_flash_init(node, &brd->flash_cap, (uint32_t)BOOT_NORMAL, ver_code);

    {
        /* kernel 分区: 只读属性 (flags = 0), 384 KiB */
        const struct nlk_flash_part_ops kernel_part = {
            .name       = "kernel",
            .max_size   = NUC472_KERNEL_MAX_SIZE,
            .erase_size = NUC472_PAGE_SIZE,
            .flags      = 0U,
            .priv       = brd,
            .prepare    = NULL,
            .write      = NULL,
            .commit     = NULL,
        };

        /* data 分区: 可读写擦除 (NLK_FLASH_PART_F_WRITABLE), 128 KiB */
        const struct nlk_flash_part_ops data_part = {
            .name       = "data",
            .max_size   = NUC472_DATA_MAX_SIZE,
            .erase_size = NUC472_PAGE_SIZE,
            .flags      = NLK_FLASH_PART_F_WRITABLE,
            .priv       = brd,
            .prepare    = nutiny_data_prepare,
            .write      = nutiny_data_write,
            .commit     = nutiny_data_commit,
        };

        (void)nlk_cap_flash_register_part(&brd->flash_cap, &kernel_part);
        (void)nlk_cap_flash_register_part(&brd->flash_cap, &data_part);
    }

    /* 挂载上行 WinUSB Bearer */
    for (i = 0; i < 20; i++) {
        if (nlk_winusb_bearer_init(node, &brd->winusb_bearer, 0U) == 0) {
            pr_info("NuTiny WinUSB bearer attached on port 0\n");
            break;
        }
        msleep(50);
    }

    return 0;
}

static int nutiny_nlk_register(void)
{
    nlk_server_register_board_setup(nutiny_nlk_setup);
    return 0;
}
SYS_INIT(nutiny_nlk_register, INIT_LEVEL_APPLICATION);
