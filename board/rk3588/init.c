/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * Rockchip RK3588 板级硬件初始化（GIC-600 + SoC 外设 + RKNPU/SMMU）
 */

#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <kernel/board.h>
#include <nos/hal_arch.h>
#include <nos/devicetree.h>
#include <soc/rk3588.h>
#include <uapi/nos/errno.h>
#include <asm/gic.h>
#include <asm/smmuv3.h>
#include <string.h>

__init void board_early_init(void)
{
}

static void rk3588_gic600_setup(void)
{
    struct arm64_gic_config cfg = {
        .gicd_phys    = 0xFE600000UL,
        .gicd_size    = 0x10000UL,
        .gicc_phys    = 0xFE600000UL,
        .gicc_size    = 0x10000UL,
        .gicr_phys    = 0xFE680000UL,
        .gicr_size    = 0x100000UL,
        .prefer_gicv3 = true,
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

__init void board_init(void)
{
    pr_info("Rockchip RK3588 Edge AI board initializing...\n");
    rk3588_gic600_setup();
    rk3588_soc_init();
    pr_info("Rockchip RK3588 board initialized\n");
}

__init void board_late_init(void)
{
#ifdef CONFIG_NOS_AI_CORE
    (void)arm64_smmuv3_probe_default();
#endif
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

int board_get_chip_uid(uint8_t uid[12])
{
    static const uint8_t rk3588_uid[12] = {
        'R', 'K', '3', '5', '8', '8', 'N', 'O', 'S', 'A', 'I', '1'
    };

    if (uid == NULL) {
        return -EINVAL;
    }
    memcpy(uid, rk3588_uid, sizeof(rk3588_uid));
    return 0;
}
