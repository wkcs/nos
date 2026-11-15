/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * QEMU Virt Multi-Arch Board Initialization
 */

#include <kernel/kernel.h>
#include <kernel/printk.h>
#include <kernel/board.h>
#include <nos/hal_arch.h>
#include <uapi/nos/errno.h>
#include <string.h>
#include <stdio.h>

__init void board_early_init(void)
{
}

__init void board_init(void)
{
    pr_info("QEMU Virt board initializing...\n");
    hal_arch_trap_init();
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

