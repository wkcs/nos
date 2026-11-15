/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 * Author: Nick Hu <huqihan@live.com>
 */
#include <kernel/kernel.h>
#include <kernel/mm.h>
#include <kernel/boot.h>
#include <kernel/errno.h>
#include <kernel/board.h>
#include <soc/nuc472_soc.h>
#include <string.h>
#include <stdio.h>
#ifdef CONFIG_NLK_SERVER
#include <nos/nlk_core.h>
#endif

#ifndef NUC472_HOST_TEST
#include <asm/irq.h>
#include <asm/mmu.h>

extern addr_t __mm_pool_start, __mm_pool_end;
extern addr_t __mm_sys_reserve_start, __mm_sys_reserve_end;
mm_reserve_node_register((addr_t)&__mm_sys_reserve_start, (addr_t)&__mm_sys_reserve_end, reserve);
mm_node_register((addr_t)&__mm_pool_start, (addr_t)&__mm_pool_end, inside);
#endif

void board_early_init(void) { }
void board_init(void)
{
#ifndef NUC472_HOST_TEST
    arch_irq_init();
    arch_mmu_init();
#endif
}
void board_late_init(void) { }

#define NUC472_RTC_XOR_KEY 0xA55A4E55UL

static int nuc472_rtc_unlock_spare(void)
{
    uint32_t timeout;

    NUC472_CLK->APBCLK0 |= NUC472_CLK_APBCLK0_RTCCKEN;

    /* 若未处于活跃初始化状态则写 INIT 激活 RTC 32.768kHz 域 */
    if ((NUC472_RTC->INIT & 0x1UL) == 0UL) {
        NUC472_RTC->INIT = NUC472_RTC_INIT_KEY;
        timeout = 400000UL;
        while ((NUC472_RTC->INIT & 0x1UL) == 0UL) {
            if (--timeout == 0UL) {
                return -ETIMEDOUT;
            }
        }
    }

    /* 写入访问密码并等待 RWENF 就绪 */
    if ((NUC472_RTC->RWEN & NUC472_RTC_RWEN_RWENF_Msk) == 0UL) {
        NUC472_RTC->RWEN = NUC472_RTC_WRITE_KEY;
        timeout = 400000UL;
        while ((NUC472_RTC->RWEN & NUC472_RTC_RWEN_RWENF_Msk) == 0UL) {
            if (--timeout == 0UL) {
                return -ETIMEDOUT;
            }
        }
    }

    /* 使能 Spare 寄存器读写访问 */
    NUC472_RTC->SPRCTL |= NUC472_RTC_SPRCTL_SPRRWEN;
    return 0;
}

int board_write_boot_flag(uint32_t flag)
{
    int ret;
    uint32_t data;

    ret = nuc472_rtc_unlock_spare();
    if (ret != 0) {
        return ret;
    }

    data = (get_boot_flag_magic(flag) == BOOT_FLAG_MAGIC) ?
           flag : get_boot_flag_data(flag);

    NUC472_RTC->SPR[0] = data;
    NUC472_RTC->SPR[1] = data ^ NUC472_RTC_XOR_KEY;
    return 0;
}

uint32_t board_read_boot_flag(void)
{
    uint32_t spr0;
    uint32_t spr1;

    NUC472_CLK->APBCLK0 |= NUC472_CLK_APBCLK0_RTCCKEN;

    spr0 = NUC472_RTC->SPR[0];
    spr1 = NUC472_RTC->SPR[1];

    if (get_boot_flag_magic(spr0) != BOOT_FLAG_MAGIC) {
        return (uint32_t)BOOT_NORMAL;
    }
    if (spr1 != (spr0 ^ NUC472_RTC_XOR_KEY)) {
        return (uint32_t)BOOT_NORMAL;
    }

    return spr0;
}

/**
 * 获取 NUC472 硬件唯一标识符 (UID)
 *
 * 通过 FMC ISP 读取 96 位 (3 个 32 位字) UID
 */
int board_get_chip_uid(uint8_t uid[12])
{
    uint32_t tmp[3] = {0};
    int ret;

    if (uid == NULL) {
        return -EINVAL;
    }

    ret = nuc472_fmc_read_uid(tmp);
    if (ret != 0) {
        return ret;
    }

    memcpy(uid, tmp, sizeof(tmp));
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

