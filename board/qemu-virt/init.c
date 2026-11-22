/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * QEMU Virt Multi-Arch Board Initialization
 */

#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <kernel/board.h>
#include <nos/hal_arch.h>
#include <nos/devicetree.h>
#include <uapi/nos/errno.h>
#include <string.h>
#include <stdio.h>

#ifdef CONFIG_ARCH_ARM64
#include <asm/gic.h>
#endif

__init void board_early_init(void)
{
}

#ifdef CONFIG_ARCH_ARM64
static void qemu_virt_arm64_gic_setup(void)
{
    struct arm64_gic_config cfg = {
        .gicd_phys    = 0x08000000UL,
        .gicd_size    = 0x10000UL,
        .gicc_phys    = 0x08010000UL,
        .gicc_size    = 0x10000UL,
        .gicr_phys    = 0x080A0000UL,
        .gicr_size    = 0xF60000UL,
#ifdef CONFIG_ARM64_GICV3
        .prefer_gicv3 = true,
#else
        .prefer_gicv3 = false,
#endif
    };

    const struct nos_dt_node *gic = nos_dt_find_compatible("arm,gic-v3");
    if (gic != NULL) {
        uint32_t d_hi = 0U, d_lo = 0U, r_hi = 0U, r_lo = 0U;
        if (nos_dt_read_u32(gic, "reg", 0U, &d_hi) == 0 &&
            nos_dt_read_u32(gic, "reg", 1U, &d_lo) == 0 &&
            nos_dt_read_u32(gic, "reg", 4U, &r_hi) == 0 &&
            nos_dt_read_u32(gic, "reg", 5U, &r_lo) == 0) {
            cfg.gicd_phys = ((uintptr_t)d_hi << 32) | (uintptr_t)d_lo;
            cfg.gicr_phys = ((uintptr_t)r_hi << 32) | (uintptr_t)r_lo;
        }
    }

    (void)arm64_gic_init(&cfg);
}
#endif

__init void board_init(void)
{
    pr_info("QEMU Virt board initializing...\n");
#ifdef CONFIG_ARCH_ARM64
    qemu_virt_arm64_gic_setup();
#else
    hal_arch_trap_init();
#endif
    pr_info("QEMU Virt board initialized\n");
}

__init void board_late_init(void)
{
}

int board_write_boot_flag(uint32_t flag)
{
    (void)flag;
    return -ENOSYS;
}

uint32_t board_read_boot_flag(void)
{
    return 0;
}

/**
 * 获取 QEMU Virt 虚拟硬件唯一标识符 (UID)
 *
 * QEMU Virt 平台无专用硬件 UID 寄存器，采用固定且唯一的虚拟 UID 标识
 */
int board_get_chip_uid(uint8_t uid[12])
{
    static const uint8_t qemu_virt_uid[12] = {
        'Q', 'E', 'M', 'U', 'V', 'I', 'R', 'T', '0', '0', '0', '1'
    };

    if (uid == NULL) {
        return -EINVAL;
    }

    memcpy(uid, qemu_virt_uid, sizeof(qemu_virt_uid));
    return 0;
}

/**
 * 获取硬件 UID 的十六进制大写字符串表示
 */
int board_get_chip_uid_hex(char *buf, size_t max_len)
{
    uint8_t uid[12] = {0};
    int ret;

    if (buf == NULL || max_len < 25) {
        return -EINVAL;
    }

    ret = board_get_chip_uid(uid);
    if (ret != 0) {
        return ret;
    }

    for (int i = 0; i < 12; i++) {
        snprintf(&buf[i * 2], 3, "%02X", uid[i]);
    }
    buf[24] = '\0';
    return 0;
}
