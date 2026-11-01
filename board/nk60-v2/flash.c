/**
 * Copyright (C) 2023-2026 胡启航 <huqihan@live.com>
 *
 * Author: 胡启航 <Nick Hu>
 */
#define pr_fmt(fmt) "[W25QXX]:%s[%d]:" fmt, __func__, __LINE__

#include <drivers/stm32_spi.h>
#include <drivers/w25q_flash.h>
#ifdef CONFIG_DEVICE_TREE
#include <drivers/spi_dt.h>
#include <nos/driver.h>
#endif
#include <board/board.h>
#include <board/boot.h>
#include <board/nk60_spi.h>
#include <fs/vfs.h>
#include <kernel/device.h>
#include <kernel/errno.h>
#include <kernel/init.h>
#include <kernel/mm.h>
#include <string.h>
#ifdef CONFIG_FATFS
#include "../../fs/fatfs/diskio.h"
#endif

struct w25qxx_info {
    struct device dev;
    struct w25q_flash flash;
    struct w25q_wl assets_wl;
    struct device assets_dev;
    bool assets_ready;
};

static struct w25qxx_info *g_nk60_flash_info;

static struct w25q_spi nk60_flash_bus = {
    .bus = &nk60_spi_bus,
    .config = { .frequency = 21000000U, .chip_select = 0, .word_size = 8, .mode = 3 },
};

static ssize_t w25qxx_read(struct device *dev, addr_t pos, void *buffer, size_t size)
{
    struct w25qxx_info *info;
    int ret;

    if (dev == NULL || pos > 0xffffffU) {
        return -EINVAL;
    }
    info = container_of(dev, struct w25qxx_info, dev);
    ret = w25q_flash_read(&info->flash, (uint32_t)pos, buffer, size);
    return ret == 0 ? (ssize_t)size : ret;
}

static ssize_t w25qxx_write(struct device *dev, addr_t pos,
                            const void *buffer, size_t size)
{
    struct w25qxx_info *info;
    int ret;

    if (dev == NULL || pos > 0xffffffU) {
        return -EINVAL;
    }
    info = container_of(dev, struct w25qxx_info, dev);
    ret = w25q_flash_write(&info->flash, (uint32_t)pos, buffer, size);
    return ret == 0 ? (ssize_t)size : ret;
}

static int w25qxx_control(struct device *dev, int cmd, void *args)
{
    struct w25qxx_info *info;
    uint32_t sector_addr;

    if (dev == NULL || args == NULL) {
        return -EINVAL;
    }
    info = container_of(dev, struct w25qxx_info, dev);
    if (cmd == W25Q_FLASH_CTRL_ERASE_SECTOR) {
        sector_addr = *(const uint32_t *)args;
        if (sector_addr > 0xffffffU) {
            return -EINVAL;
        }
        return w25q_flash_erase(&info->flash, sector_addr, 4096U);
    }
    return -EINVAL;
}

#ifdef CONFIG_DEVICE_TREE
static int nk60_flash_configure(const struct nos_dt_node *node)
{
    struct spi_dt_resources resources;
    uint32_t spi_address;
    uint32_t gpio_address;

    if (!nos_dt_is_compatible(node, "jedec,spi-nor") ||
        spi_dt_resolve(node, &resources) != 0 ||
        !nos_dt_is_compatible(resources.bus, "st,stm32f4-spi") ||
        !nos_dt_is_compatible(resources.gpio, "st,stm32f4-gpio") ||
        nos_dt_read_u32(resources.bus, "reg", 0, &spi_address) != 0 ||
        nos_dt_read_u32(resources.gpio, "reg", 0, &gpio_address) != 0 ||
        spi_address != (uint32_t)(uintptr_t)SPI1 ||
        gpio_address != (uint32_t)(uintptr_t)GPIOB ||
        resources.gpio_pin != 2 || resources.gpio_flags != 1 ||
        resources.config.chip_select != 0 ||
        (resources.config.mode != 0 && resources.config.mode != 3)) {
        return -EINVAL;
    }
    nk60_flash_bus.config = resources.config;
    return 0;
}
#endif

static int w25qxx_capacity(uint16_t id, uint32_t *capacity)
{
    if ((id >> 8) != 0xefU || (id & 0xffU) < 0x13U || (id & 0xffU) > 0x17U) {
        return -ENODEV;
    }
    *capacity = 1U << ((id & 0xffU) + 1U);
    return 0;
}

static int w25qxx_probe(struct device *dev)
{
    struct w25qxx_info *info = container_of(dev, struct w25qxx_info, dev);
    uint32_t capacity;
    uint16_t id;
    int ret;

    if (!device_is_ready(&nk60_spi_bus)) {
        ret = stm32_spi_init(&nk60_spi_bus);
        if (ret != 0) return ret;
    }
    ret = w25q_spi_read_id(&nk60_flash_bus, &id);
    if (ret != 0) return ret;
    ret = w25qxx_capacity(id, &capacity);
    if (ret != 0) return ret;
    ret = w25q_flash_init(&info->flash, &nk60_flash_bus, capacity);
    if (ret != 0) return ret;
    pr_info("w25qxx: chip id = 0x%x, capacity = %u bytes\n", id, capacity);
    return 0;
}

#ifdef CONFIG_DEVICE_TREE
static int w25qxx_dt_probe(struct device *dev, const struct nos_dt_node *node,
                           const void *match_data)
{
    int ret;

    (void)match_data;
    ret = nk60_flash_configure(node);
    return ret == 0 ? w25qxx_probe(dev) : ret;
}

static const struct nos_dt_match w25qxx_matches[] = {
    { .compatible = "jedec,spi-nor" },
};

static const struct nos_dt_driver w25qxx_driver = {
    .name = "w25qxx",
    .matches = w25qxx_matches,
    .match_count = sizeof(w25qxx_matches) / sizeof(w25qxx_matches[0]),
    .probe = w25qxx_dt_probe,
};
#endif

struct w25q_wl *nk60_flash_get_assets_wl(void)
{
    if (g_nk60_flash_info == NULL || !g_nk60_flash_info->assets_ready) {
        return NULL;
    }
    return &g_nk60_flash_info->assets_wl;
}

int nk60_assets_mount_fatfs(void)
{
#if defined(CONFIG_VFS) && defined(CONFIG_FATFS)
    struct dentry *root;
    struct dentry *dir = NULL;
    struct dentry *mnt_root;

    if (g_nk60_flash_info == NULL || !g_nk60_flash_info->assets_ready) {
        return -ENODEV;
    }

    fatfs_set_block_device(&g_nk60_flash_info->assets_dev);
    root = vfs_get_root();
    if (root == NULL) {
        return -ENOENT;
    }

    (void)vfs_make_dir(root, "assets", 0755, &dir);
    if (dir == NULL) {
        dir = vfs_lookup_path(root, "/assets");
    }
    if (dir == NULL) {
        return -ENOENT;
    }

    (void)vfs_unmount(dir);
    mnt_root = vfs_mount("fatfs", 0, "nk60_v2-assets", NULL);
    if (mnt_root == NULL) {
        pr_err("w25qxx: failed to mount FatFs on /assets\n");
        return -EIO;
    }
    if (vfs_bind_mount(dir, mnt_root) != 0) {
        pr_err("w25qxx: failed to bind /assets mount\n");
        return -EIO;
    }

    pr_info("w25qxx: mounted wear-leveled assets FatFs at /assets (%u KB, %u spares)\n",
            (unsigned int)(g_nk60_flash_info->assets_wl.logical_blocks * 4U),
            (unsigned int)g_nk60_flash_info->assets_wl.spare_sectors);
    return 0;
#else
    return 0;
#endif
}

int nk60_assets_reload(void)
{
    int ret;

    if (g_nk60_flash_info == NULL || !g_nk60_flash_info->assets_ready) {
        return -ENODEV;
    }
    ret = w25q_wl_reset(&g_nk60_flash_info->assets_wl);
    if (ret != 0) {
        return ret;
    }
    return nk60_assets_mount_fatfs();
}

int spi_w25qxx_init(void)
{
    struct w25qxx_info *info;
    int ret;
#ifdef CONFIG_DEVICE_TREE
    const struct nos_dt_node *chosen = nos_dt_find_path("/chosen");
    const struct nos_dt_node *node;
    uint32_t phandle;

    if (nos_dt_read_u32(chosen, "nos,flash", 0, &phandle) != 0) return -EINVAL;
    node = nos_dt_find_phandle(phandle);
    if (node == NULL) return -ENODEV;
#endif
    info = kmalloc(sizeof(*info), GFP_KERNEL);
    if (info == NULL) return -ENOMEM;
    memset(info, 0, sizeof(*info));
    ret = device_init(&info->dev);
    if (ret == 0) {
        info->dev.name = "nk60_v2-flash";
        info->dev.ops.write = w25qxx_write;
        info->dev.ops.read = w25qxx_read;
        info->dev.ops.control = w25qxx_control;
#ifdef CONFIG_DEVICE_TREE
        ret = nos_dt_device_bind(&info->dev, node, &w25qxx_driver);
#else
        ret = w25qxx_probe(&info->dev);
#endif
    }
    if (ret == 0) ret = device_register(&info->dev);
    if (ret != 0) {
        kfree(info);
        return ret;
    }

    g_nk60_flash_info = info;
    if (info->flash.capacity > NK60_W25Q_ASSETS_ADDR) {
        uint32_t assets_region = info->flash.capacity - NK60_W25Q_ASSETS_ADDR;

        if (w25q_wl_init(&info->assets_wl, &info->flash,
                         NK60_W25Q_ASSETS_ADDR, assets_region) == 0 &&
            device_init(&info->assets_dev) == 0) {
            info->assets_dev.name = "nk60_v2-assets";
            info->assets_dev.data = &info->assets_wl;
            info->assets_dev.api = &w25q_wl_block_driver_api;
            info->assets_dev.ready = true;
            if (device_register(&info->assets_dev) == 0) {
                info->assets_ready = true;
                (void)nk60_assets_mount_fatfs();
            }
        }
    }
    return 0;
}
task_init(spi_w25qxx_init);
