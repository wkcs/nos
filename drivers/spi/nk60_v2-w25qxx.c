/**
 * Copyright (C) 2023-2026 胡启航 <huqihan@live.com>
 *
 * Author: 胡启航 <Nick Hu>
 */
#define pr_fmt(fmt) "[W25QXX]:%s[%d]:" fmt, __func__, __LINE__

#include <drivers/stm32_spi.h>
#include <drivers/w25q_flash.h>
#ifdef CONFIG_DEVICE_TREE
#include <drivers/w25q_dt.h>
#endif
#include <board/board.h>
#include <board/nk60_spi.h>
#include <kernel/device.h>
#include <kernel/errno.h>
#include <kernel/init.h>
#include <kernel/mm.h>
#include <string.h>

struct w25qxx_info {
    struct device dev;
    struct w25q_flash flash;
};

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

static int nk60_flash_configure(void)
{
#ifdef CONFIG_DEVICE_TREE
    const struct nos_dt_node *chosen = nos_dt_find_path("/chosen");
    const struct nos_dt_node *node;
    struct w25q_dt_resources resources;
    uint32_t phandle;
    uint32_t spi_address;
    uint32_t gpio_address;

    if (nos_dt_read_u32(chosen, "nos,flash", 0, &phandle) != 0) {
        return -EINVAL;
    }
    node = nos_dt_find_phandle(phandle);
    if (w25q_dt_resolve(node, &resources) != 0 ||
        !nos_dt_is_compatible(resources.bus, "st,stm32f4-spi") ||
        !nos_dt_is_compatible(resources.gpio, "st,stm32f4-gpio") ||
        nos_dt_read_u32(resources.bus, "reg", 0, &spi_address) != 0 ||
        nos_dt_read_u32(resources.gpio, "reg", 0, &gpio_address) != 0 ||
        spi_address != (uint32_t)(uintptr_t)SPI1 ||
        gpio_address != (uint32_t)(uintptr_t)GPIOB ||
        resources.gpio_pin != 2 || resources.gpio_flags != 1 ||
        resources.config.chip_select != 0 || resources.config.mode != 3 ||
        resources.config.frequency != 21000000U) {
        return -EINVAL;
    }
    nk60_flash_bus.config = resources.config;
#endif
    return 0;
}

static int w25qxx_capacity(uint16_t id, uint32_t *capacity)
{
    if ((id >> 8) != 0xefU || (id & 0xffU) < 0x13U || (id & 0xffU) > 0x17U) {
        return -ENODEV;
    }
    *capacity = 1U << ((id & 0xffU) + 1U);
    return 0;
}

int spi_w25qxx_init(void)
{
    struct w25qxx_info *info;
    uint32_t capacity;
    uint16_t id;
    int ret;

    ret = nk60_flash_configure();
    if (ret != 0) return ret;
    ret = stm32_spi_init(&nk60_spi_bus);
    if (ret != 0) return ret;
    ret = w25q_spi_read_id(&nk60_flash_bus, &id);
    if (ret != 0) return ret;
    ret = w25qxx_capacity(id, &capacity);
    if (ret != 0) return ret;

    info = kmalloc(sizeof(*info), GFP_KERNEL);
    if (info == NULL) return -ENOMEM;
    memset(info, 0, sizeof(*info));
    ret = device_init(&info->dev);
    if (ret == 0) ret = w25q_flash_init(&info->flash, &nk60_flash_bus, capacity);
    if (ret != 0) {
        kfree(info);
        return ret;
    }
    info->dev.name = "nk60_v2-flash";
    info->dev.ops.write = w25qxx_write;
    info->dev.ops.read = w25qxx_read;
    ret = device_register(&info->dev);
    if (ret != 0) {
        kfree(info);
        return ret;
    }
    pr_info("w25qxx: chip id = 0x%x, capacity = %u bytes\r\n", id, capacity);
    return 0;
}
task_init(spi_w25qxx_init);
