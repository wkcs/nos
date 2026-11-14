/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 系统级别初始化与 FMC UID 读取
 */
#include <stddef.h>
#include <stdint.h>
#include <soc/nuc472_soc.h>

/**
 * @brief 系统硬件初始化，由 start.S 或 soc_early_init 触发
 */
void SystemInit(void)
{
    /* 开启 Cortex-M4F 硬件 FPU 访问权限 (CP10 与 CP11) */
#if defined(__VFP_FP__) && !defined(__SOFTFP__)
    *(volatile uint32_t *)0xE000ED88UL |= (0xFU << 20);
    __asm__ volatile ("dsb\n isb" ::: "memory");
#endif

    /* 84 MHz PLL 时钟配置与故障安全回退 */
    nuc472_clk_init_84mhz();

    /* 板级外设默认引脚复用配置 */
    nuc472_mfp_config_board_defaults();
}

/**
 * @brief 读取 NUC472 96-bit 硬件唯一芯片 ID (UID)
 *
 * @param uid 存放 3 个 32-bit 字的输出数组
 * @return 0 成功，-1 参数错误或 ISP 操作失败
 */
int nuc472_fmc_read_uid(uint32_t uid[3])
{
    if (uid == NULL) {
        return -1;
    }

    nuc472_sys_unlock();
    NUC472_FMC->ISPCTL |= NUC472_FMC_ISPCTL_ISPEN;

    for (uint32_t i = 0; i < 3U; i++) {
        uint32_t timeout = 100000UL;

        NUC472_FMC->ISPCMD = NUC472_FMC_ISPCMD_READ_UID;
        NUC472_FMC->ISPADDR = i * 4U;
        NUC472_FMC->ISPTRG = NUC472_FMC_ISPTRG_ISPGO;

        while (((NUC472_FMC->ISPTRG & NUC472_FMC_ISPTRG_ISPGO) != 0U) && (timeout > 0UL)) {
            timeout--;
        }

        if (timeout == 0UL) {
            NUC472_FMC->ISPCTL &= ~NUC472_FMC_ISPCTL_ISPEN;
            nuc472_sys_lock();
            return -1;
        }

        if ((NUC472_FMC->ISPCTL & NUC472_FMC_ISPCTL_ISPFF) != 0U) {
            NUC472_FMC->ISPCTL |= NUC472_FMC_ISPCTL_ISPFF;
            NUC472_FMC->ISPCTL &= ~NUC472_FMC_ISPCTL_ISPEN;
            nuc472_sys_lock();
            return -1;
        }

        uid[i] = NUC472_FMC->ISPDAT;
    }

    NUC472_FMC->ISPCTL &= ~NUC472_FMC_ISPCTL_ISPEN;
    nuc472_sys_lock();
    return 0;
}
