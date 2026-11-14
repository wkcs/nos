/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 片内 FMC ISP 底层原语 (含 SRAM .ramfunc 执行与 MPU 安全切换)
 */
#include <soc/nuc472_soc.h>
#include <kernel/types.h>
#include <kernel/irq.h>
#include <kernel/errno.h>

#define ARMV7M_MPU_CTRL_REG (*(volatile uint32_t *)0xE000ED94UL)

#ifndef NUC472_HOST_TEST
__attribute__((section(".ramfunc"), noinline))
static int fmc_isp_trigger_ramfunc(void)
{
    uint32_t timeout = 1000000UL;

    NUC472_FMC->ISPTRG = NUC472_FMC_ISPTRG_ISPGO;
    /* 双重内存/指令屏障确保 CPU 从 SRAM 等待 ISP 状态清零 */
    __asm__ volatile ("isb\n dsb\n" ::: "memory");
    while ((NUC472_FMC->ISPTRG & NUC472_FMC_ISPTRG_ISPGO) != 0UL) {
        if (--timeout == 0UL) {
            return -ETIMEDOUT;
        }
    }
    if ((NUC472_FMC->ISPCTL & NUC472_FMC_ISPCTL_ISPFF) != 0UL) {
        NUC472_FMC->ISPCTL |= NUC472_FMC_ISPCTL_ISPFF;
        return -EIO;
    }
    return 0;
}
#else
int fmc_isp_trigger_ramfunc(void); /* 由测试桩拦截 */
#endif

static int fmc_exec_isp(uint32_t cmd, uint32_t addr, uint32_t dat_in,
                        uint32_t *dat_out, bool aprom_update)
{
    addr_t irq_flags;
    uint32_t mpu_ctrl_saved = 0U;
    int ret;

    irq_flags = disable_irq_save();
#ifndef NUC472_HOST_TEST
    /* 因 SRAM 默认设置 XN=1，执行 .ramfunc 前在关中断下临时关闭 MPU 并恢复 */
    mpu_ctrl_saved = ARMV7M_MPU_CTRL_REG;
    if ((mpu_ctrl_saved & 1UL) != 0UL) {
        ARMV7M_MPU_CTRL_REG = mpu_ctrl_saved & ~1UL;
        __asm__ volatile ("dsb\n isb\n" ::: "memory");
    }
#endif
    nuc472_sys_unlock();
    NUC472_CLK->AHBCLK |= NUC472_CLK_AHBCLK_ISPCKEN;
    NUC472_FMC->ISPCTL |= NUC472_FMC_ISPCTL_ISPEN;
    if ((NUC472_FMC->ISPCTL & NUC472_FMC_ISPCTL_ISPFF) != 0UL) {
        NUC472_FMC->ISPCTL |= NUC472_FMC_ISPCTL_ISPFF;
    }
    if (aprom_update) {
        NUC472_FMC->ISPCTL |= NUC472_FMC_ISPCTL_APUEN;
    } else {
        NUC472_FMC->ISPCTL &= ~NUC472_FMC_ISPCTL_APUEN;
    }
    NUC472_FMC->ISPCMD = cmd;
    NUC472_FMC->ISPADDR = addr;
    NUC472_FMC->ISPDAT = dat_in;

    ret = fmc_isp_trigger_ramfunc();
    if (ret == 0 && dat_out != NULL) {
        *dat_out = NUC472_FMC->ISPDAT;
    }

    NUC472_FMC->ISPCTL &= ~(NUC472_FMC_ISPCTL_APUEN | NUC472_FMC_ISPCTL_ISPEN);
    nuc472_sys_lock();
#ifndef NUC472_HOST_TEST
    if ((mpu_ctrl_saved & 1UL) != 0UL) {
        ARMV7M_MPU_CTRL_REG = mpu_ctrl_saved;
        __asm__ volatile ("dsb\n isb\n" ::: "memory");
    }
#else
    (void)mpu_ctrl_saved;
#endif
    enable_irq_save(irq_flags);
    return ret;
}

int nuc472_fmc_erase_page(uint32_t addr)
{
    if ((addr & (NUC472_FMC_PAGE_SIZE - 1UL)) != 0UL) {
        return -EINVAL;
    }
    return fmc_exec_isp(NUC472_FMC_ISPCMD_PAGE_ERASE, addr, 0U, NULL, true);
}

int nuc472_fmc_write_u32(uint32_t addr, uint32_t val)
{
    if ((addr & 3UL) != 0UL) {
        return -EINVAL;
    }
    return fmc_exec_isp(NUC472_FMC_ISPCMD_PROGRAM, addr, val, NULL, true);
}

__attribute__((weak)) int nuc472_fmc_read_uid(uint32_t uid[3])
{
    if (uid == NULL) {
        return -EINVAL;
    }
    for (uint32_t i = 0U; i < 3U; i++) {
        int ret = fmc_exec_isp(NUC472_FMC_ISPCMD_READ_UID, i * 4U, 0U, &uid[i], false);
        if (ret != 0) {
            return ret;
        }
    }
    return 0;
}

int nuc472_fmc_read_id(uint32_t *cid, uint32_t *pid)
{
    int ret;

    if (cid != NULL) {
        ret = fmc_exec_isp(NUC472_FMC_ISPCMD_READ_CID, 0x00U, 0U, cid, false);
        if (ret != 0) {
            return ret;
        }
    }
    if (pid != NULL) {
        ret = fmc_exec_isp(NUC472_FMC_ISPCMD_READ_PID, 0x04U, 0U, pid, false);
        if (ret != 0) {
            return ret;
        }
    }
    return 0;
}
