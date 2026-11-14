/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 系统多功能引脚复用 (MFP) 配置
 */
#include <soc/nuc472_soc.h>

void nuc472_mfp_set(volatile uint32_t *reg, uint32_t mask, uint32_t val)
{
    *reg = (*reg & ~mask) | (val & mask);
}

void nuc472_mfp_config_board_defaults(void)
{
    nuc472_sys_unlock();

    /* PG.1 / PG.2: UART0 RXD / TXD */
    nuc472_mfp_set(&NUC472_SYS->GPG_MFPL,
                   NUC472_SYS_MFPL_Msk(1U) | NUC472_SYS_MFPL_Msk(2U),
                   NUC472_SYS_MFPL_Val(1U, 1U) | NUC472_SYS_MFPL_Val(2U, 1U));

    /* PB.10: 板载 LED GPIO */
    nuc472_mfp_set(&NUC472_SYS->GPB_MFPH,
                   NUC472_SYS_MFPH_Msk(10U),
                   NUC472_SYS_MFPH_Val(10U, 0U));

    /* PE.2 ~ PE.5: SPI0 (SS0, CLK, MISO0, MOSI0) */
    nuc472_mfp_set(&NUC472_SYS->GPE_MFPL,
                   NUC472_SYS_MFPL_Msk(2U) | NUC472_SYS_MFPL_Msk(3U) |
                   NUC472_SYS_MFPL_Msk(4U) | NUC472_SYS_MFPL_Msk(5U),
                   NUC472_SYS_MFPL_Val(2U, 1U) | NUC472_SYS_MFPL_Val(3U, 1U) |
                   NUC472_SYS_MFPL_Val(4U, 1U) | NUC472_SYS_MFPL_Val(5U, 1U));

    /* PH.3 / PH.4: I2C3 SCL / SDA */
    nuc472_mfp_set(&NUC472_SYS->GPH_MFPL,
                   NUC472_SYS_MFPL_Msk(3U) | NUC472_SYS_MFPL_Msk(4U),
                   NUC472_SYS_MFPL_Val(3U, 3U) | NUC472_SYS_MFPL_Val(4U, 3U));

    /* PA.5 / PA.6: PWM0 CH0 / CH1 */
    nuc472_mfp_set(&NUC472_SYS->GPA_MFPL,
                   NUC472_SYS_MFPL_Msk(5U) | NUC472_SYS_MFPL_Msk(6U),
                   NUC472_SYS_MFPL_Val(5U, 2U) | NUC472_SYS_MFPL_Val(6U, 2U));

    nuc472_sys_lock();
}
