/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 片上外设 (PWM0, EADC, TIMER0, RTC, WDT, EMAC MDIO) 主机回归测试 (TDD)
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUC472_HOST_TEST 1
#include <kernel/types.h>
#include <soc/nuc472_regs.h>
#include <soc/nuc472_periph.h>

/* 模拟片上硬件寄存器实例 */
static NUC472_SYS_Type   g_test_sys;
static NUC472_CLK_Type   g_test_clk;
static NUC472_PWM_Type   g_test_pwm0;
static NUC472_EADC_Type  g_test_eadc;
static NUC472_TIMER_Type g_test_tmr0;
static NUC472_RTC_Type   g_test_rtc;
static NUC472_WDT_Type   g_test_wdt;
static NUC472_EMAC_Type  g_test_emac;

#define NUC472_SYS        (&g_test_sys)
#define NUC472_CLK        (&g_test_clk)
#define NUC472_PWM0       (&g_test_pwm0)
#define NUC472_EADC       (&g_test_eadc)
#define NUC472_TMR0       (&g_test_tmr0)
#define NUC472_RTC        (&g_test_rtc)
#define NUC472_WDT        (&g_test_wdt)
#define NUC472_EMAC       (&g_test_emac)

/* 辅助宏：用于主机测试向硬件只读 (__I 即 const volatile) 寄存器注入模拟值 */
#define HOST_REG_WRITE(reg_member, val) \
    (*(volatile uint32_t *)(uintptr_t)&(reg_member) = (uint32_t)(val))

#include <soc/nuc472_soc.h>
#include <kernel/errno.h>
#include <drivers/nuc472_periphs.h>

/* 模拟系统与寄存器函数桩 */
NUC472_SYS_Type *nuc472_test_sys(void) { return &g_test_sys; }
NUC472_CLK_Type *nuc472_test_clk(void) { return &g_test_clk; }
NUC472_FMC_Type *nuc472_test_fmc(void) { return NULL; }
NUC472_RTC_Type *nuc472_test_rtc(void) { return &g_test_rtc; }

uint32_t nuc472_clk_get_pclk_hz(void)
{
    return 84000000UL;
}

/* 模拟系统解锁/上锁状态标志 */
static bool g_sys_unlocked = false;
void test_set_sys_lock_state(bool unlocked) { g_sys_unlocked = unlocked; }

/* 模拟 RTC RWEN 解锁等待计数 */
static uint32_t g_rtc_rwen_key_writes = 0;

/* 模拟中断临界区与 NVIC 函数桩 */
static int g_irq_disabled_count = 0;
addr_t disable_irq_save(void)
{
    g_irq_disabled_count++;
    return 0;
}

void enable_irq_save(addr_t flags)
{
    (void)flags;
    g_irq_disabled_count--;
}

static uint32_t g_test_nvic_iser[4];
static uint32_t g_test_nvic_icer[4];

void nuc472_nvic_enable_irq(uint32_t irqn)
{
    g_test_nvic_iser[irqn >> 5] |= (1UL << (irqn & 0x1FU));
}

void nuc472_nvic_disable_irq(uint32_t irqn)
{
    g_test_nvic_icer[irqn >> 5] |= (1UL << (irqn & 0x1FU));
    g_test_nvic_iser[irqn >> 5] &= ~(1UL << (irqn & 0x1FU));
}

void nuc472_nvic_clear_pending_irq(uint32_t irqn)
{
    (void)irqn;
}

void TMR0_IRQHandler(void);

/* 包含实现文件以进行直接单元测试 */
#include "../../soc/nuc472/sys_mfp.c"
#include "../../drivers/misc/adc_nuc472.c"
#include "../../drivers/misc/pwm_nuc472.c"
#include "../../drivers/misc/timer_nuc472.c"
#include "../../drivers/misc/rtc_nuc472.c"
#include "../../drivers/misc/wdt_nuc472.c"
#include "../../drivers/misc/emac_probe_nuc472.c"

/* =========================================================================
 * 1. EADC 内部带隙 (VBG) 与温度传感器 (VTEMP) 测试
 * ========================================================================= */
static void test_eadc_periph(void)
{
    printf("[TEST] EADC 内部带隙与温度传感器驱动测试...\n");
    memset(&g_test_eadc, 0, sizeof(g_test_eadc));
    memset(&g_test_clk, 0, sizeof(g_test_clk));
    memset(&g_test_sys, 0, sizeof(g_test_sys));

    int ret = nuc472_eadc_init();
    assert(ret == 0);

    /* 校验时钟、VREFCTL、TEMPCTL 与 EADC 核心使能 */
    assert((g_test_clk.APBCLK1 & NUC472_CLK_APBCLK1_EADCCKEN) != 0UL);
    assert((g_test_clk.CLKSEL1 & NUC472_CLK_CLKSEL1_ADCSEL_Msk) == NUC472_CLK_CLKSEL1_ADCSEL_HXT);
    assert(((g_test_clk.CLKDIV0 & NUC472_CLK_CLKDIV0_ADCDIV_Msk) >> NUC472_CLK_CLKDIV0_ADCDIV_Pos) == 0U);
    assert((g_test_sys.VREFCTL & NUC472_SYS_VREFCTL_ADCMODESEL_Msk) != 0UL);
    assert((g_test_sys.TEMPCTL & NUC472_SYS_TEMPCTL_VTEMPEN_Msk) != 0UL);
    assert((g_test_eadc.CTL & NUC472_EADC_CTL_ADCEN) != 0UL);

    /* 1.1 采样 VBG (通道 8) */
    /* 模拟硬件转换就绪并返回 1489 码 (1489 * 3300 / 4095 = 1200 mV) */
    HOST_REG_WRITE(g_test_eadc.STATUS0, 1U);
    HOST_REG_WRITE(g_test_eadc.AD0DAT0, 1489U);

    uint32_t mv = 0;
    ret = nuc472_eadc_read_vbg_mv(&mv);
    assert(ret == 0);
    assert((g_test_eadc.AD0SPCTL0 & 0x1FU) == NUC472_EADC_CH_VBG);
    assert(mv == 1200UL);

    /* 1.2 采样 VTEMP (通道 9) */
    /* 模拟硬件转换就绪并返回 868 码 (868 * 3300 / 4095 = 700 mV -> 25 摄氏度) */
    HOST_REG_WRITE(g_test_eadc.STATUS0, 1U);
    HOST_REG_WRITE(g_test_eadc.AD0DAT0, 868U);

    int32_t temp_c = 0;
    ret = nuc472_eadc_read_temp_celsius(&temp_c);
    assert(ret == 0);
    assert((g_test_eadc.AD0SPCTL0 & 0x1FU) == NUC472_EADC_CH_VTEMP);
    assert(temp_c == 25);

    /* 1.3 模拟未就绪超时 */
    HOST_REG_WRITE(g_test_eadc.STATUS0, 0U); /* 保持未就绪 */
    ret = nuc472_eadc_read_vbg_mv(&mv);
    assert(ret == -ETIMEDOUT);

    printf("  -> EADC 测试通过!\n");
}

/* =========================================================================
 * 2. PWM0 波形输出测试
 * ========================================================================= */
static void test_pwm0_periph(void)
{
    printf("[TEST] PWM0 输出配置与使能测试...\n");
    memset(&g_test_pwm0, 0, sizeof(g_test_pwm0));
    memset(&g_test_clk, 0, sizeof(g_test_clk));
    memset(&g_test_sys, 0, sizeof(g_test_sys));

    int ret = nuc472_pwm_init(0);
    assert(ret == 0);
    assert((g_test_clk.APBCLK1 & NUC472_CLK_APBCLK1_PWM0CH01CKEN) != 0UL);
    /* PA.5 复用模式应为 PWM (功能号 2) */
    assert((g_test_sys.GPA_MFPL & NUC472_SYS_MFPL_Msk(5U)) == NUC472_SYS_MFPL_Val(5U, 2U));

    /* 配置通道 0 为 10 kHz、占空比 50% */
    ret = nuc472_pwm_config(0, 10000, 50);
    assert(ret == 0);

    /* 校验周期与占空比 */
    uint32_t period = g_test_pwm0.PERIOD[0] + 1U;
    uint32_t cmp = g_test_pwm0.CMPDAT[0] + 1U;
    assert(period > 0);
    /* 占空比比例 CMP/PERIOD 约为 50% (误差允许 1%) */
    uint32_t duty_calc = (cmp * 100U) / period;
    assert(duty_calc >= 49U && duty_calc <= 51U);

    /* 校验自动重载模式、CNTEN 与 POEN 置位 */
    assert((g_test_pwm0.CTL & NUC472_PWM_CTL_CNTMODE_AUTO(0)) != 0UL);
    assert((g_test_pwm0.CNTEN & NUC472_PWM_CNTEN_CH(0)) != 0UL);
    assert((g_test_pwm0.POEN & NUC472_PWM_POEN_CH(0)) != 0UL);

    /* 停止通道 0 */
    ret = nuc472_pwm_stop(0);
    assert(ret == 0);
    assert((g_test_pwm0.CNTEN & NUC472_PWM_CNTEN_CH(0)) == 0UL);
    assert((g_test_pwm0.POEN & NUC472_PWM_POEN_CH(0)) == 0UL);

    /* 验证不支持的通道 ch >= 2 显式返回 -ENOTSUP */
    assert(nuc472_pwm_init(2U) == -ENOTSUP);
    assert(nuc472_pwm_config(2U, 10000, 50) == -ENOTSUP);
    assert(nuc472_pwm_start(2U) == -ENOTSUP);
    assert(nuc472_pwm_stop(2U) == -ENOTSUP);

    printf("  -> PWM0 测试通过!\n");
}

/* =========================================================================
 * 3. TIMER0 高精度微秒/滴答定时器测试
 * ========================================================================= */
static void test_timer0_periph(void)
{
    printf("[TEST] TIMER0 周期模式与滴答累加测试...\n");
    memset(&g_test_tmr0, 0, sizeof(g_test_tmr0));
    memset(&g_test_clk, 0, sizeof(g_test_clk));

    /* 初始化为 1000 Hz (1 ms 中断周期) */
    int ret = nuc472_timer0_init(1000);
    assert(ret == 0);
    assert((g_test_clk.APBCLK0 & NUC472_CLK_APBCLK0_TMR0CKEN) != 0UL);
    assert((g_test_tmr0.CTL & NUC472_TIMER_CTL_MODE_PERIODIC) != 0UL);
    assert((g_test_tmr0.CTL & NUC472_TIMER_CTL_INTEN) != 0UL);
    assert(g_test_tmr0.CMP > 0UL);

    ret = nuc472_timer0_start();
    assert(ret == 0);
    assert((g_test_tmr0.CTL & NUC472_TIMER_CTL_CNTEN) != 0UL);
    /* 确认启动时开启 NVIC IRQ 32 (TMR0) */
    assert((g_test_nvic_iser[NUC472_TMR0_IRQn >> 5] & (1UL << (NUC472_TMR0_IRQn & 0x1FU))) != 0UL);

    uint64_t ticks_before = nuc472_timer0_get_ticks();

    /* 模拟硬件触发超时中断并由强符号 TMR0_IRQHandler 处理 */
    g_test_tmr0.INTSTS = NUC472_TIMER_INTSTS_TIF;
    TMR0_IRQHandler();

    /* 确认写 1 清零 TIF 且软件滴答递增 */
    assert(g_test_tmr0.INTSTS == NUC472_TIMER_INTSTS_TIF); /* 模拟寄存器由驱动写 1 清零 */
    assert(nuc472_timer0_get_ticks() == ticks_before + 1U);

    /*
     * 模拟跨周期溢出窗口：CNT 已回绕但中断尚未响应 (TIF=1, CNT=10)
     * 正常上一周期末尾 (CNT=995, TIF=0)
     */
    g_test_tmr0.INTSTS = 0;
    HOST_REG_WRITE(g_test_tmr0.CNT, 995);
    uint64_t us_prev = nuc472_timer0_get_us();

    /* 模拟时钟到达比较值并回绕 (CNT=10, TIF=1, ticks 尚未被 ISR 递增) */
    g_test_tmr0.INTSTS = NUC472_TIMER_INTSTS_TIF;
    HOST_REG_WRITE(g_test_tmr0.CNT, 10);
    uint64_t us_curr = nuc472_timer0_get_us();

    /* 必须补偿单周期滴答，确保微秒时间戳单调递增，绝不逆流倒退 */
    assert(us_curr > us_prev);
    assert(us_curr == us_prev + 15U);

    ret = nuc472_timer0_stop();
    assert(ret == 0);
    assert((g_test_tmr0.CTL & NUC472_TIMER_CTL_CNTEN) == 0UL);
    /* 确认停止时禁用 NVIC IRQ 32 (TMR0) */
    assert((g_test_nvic_icer[NUC472_TMR0_IRQn >> 5] & (1UL << (NUC472_TMR0_IRQn & 0x1FU))) != 0UL);

    printf("  -> TIMER0 测试通过!\n");
}

/* =========================================================================
 * 4. RTC 日历时钟与 SPR 隔离测试
 * ========================================================================= */
static void test_rtc_periph(void)
{
    printf("[TEST] RTC 日历时钟 BCD 与后备寄存器隔离测试...\n");
    memset(&g_test_rtc, 0, sizeof(g_test_rtc));
    memset(&g_test_clk, 0, sizeof(g_test_clk));

    /* 预设后备寄存器，模拟已有 boot_flag */
    g_test_rtc.SPR[0] = 0x12345678UL;
    g_test_rtc.SPR[1] = 0x87654321UL;

    /* 模拟已就绪 */
    g_test_rtc.INIT = 1U;
    g_test_rtc.RWEN = NUC472_RTC_RWEN_RWENF_Msk;

    int ret = nuc472_rtc_init();
    assert(ret == 0);
    assert((g_test_clk.APBCLK0 & NUC472_CLK_APBCLK0_RTCCKEN) != 0UL);
    /* 严禁误改或清零 SPR[0..1] */
    assert(g_test_rtc.SPR[0] == 0x12345678UL);
    assert(g_test_rtc.SPR[1] == 0x87654321UL);

    /* 设置时间：2026-11-14 20:30:00 (周六 weekday=6) */
    struct nuc472_rtc_time t_in = {
        .year = 2026,
        .month = 11,
        .day = 14,
        .hour = 20,
        .minute = 30,
        .second = 0,
        .weekday = 6,
    };

    g_rtc_rwen_key_writes = 0;
    ret = nuc472_rtc_set_time(&t_in);
    assert(ret == 0);
    /* 每次写 CAL, TIME, WEEKDAY 均必须先获得 RWEN 同步许可 */
    assert(g_rtc_rwen_key_writes >= 3U);

    /* 验证 CAL BCD: 年 26 (0x26), 月 11 (0x11), 日 14 (0x14) */
    /* CAL: [23:20]=2, [19:16]=6, [12]=1, [11:8]=1, [5:4]=1, [3:0]=4 */
    assert(((g_test_rtc.CAL >> 20) & 0xFU) == 2U);
    assert(((g_test_rtc.CAL >> 16) & 0xFU) == 6U);
    assert(((g_test_rtc.CAL >> 8) & 0x1FU) == 0x11U);
    assert((g_test_rtc.CAL & 0x3FU) == 0x14U);

    /* 验证 TIME BCD: 时 20 (0x20), 分 30 (0x30), 秒 00 (0x00) */
    assert(((g_test_rtc.TIME >> 16) & 0x3FU) == 0x20U);
    assert(((g_test_rtc.TIME >> 8) & 0x7FU) == 0x30U);
    assert((g_test_rtc.TIME & 0x7FU) == 0x00U);

    /* 读取时间并校验解码 */
    struct nuc472_rtc_time t_out;
    memset(&t_out, 0, sizeof(t_out));
    ret = nuc472_rtc_get_time(&t_out);
    assert(ret == 0);
    assert(t_out.year == 2026);
    assert(t_out.month == 11);
    assert(t_out.day == 14);
    assert(t_out.hour == 20);
    assert(t_out.minute == 30);
    assert(t_out.second == 0);
    assert(t_out.weekday == 6);

    /* 再次确认 SPR 后备寄存器安全 */
    assert(g_test_rtc.SPR[0] == 0x12345678UL);
    assert(g_test_rtc.SPR[1] == 0x87654321UL);

    printf("  -> RTC 测试通过!\n");
}

/* =========================================================================
 * 5. WDT 看门狗驱动测试
 * ========================================================================= */
static void test_wdt_periph(void)
{
    printf("[TEST] WDT 看门狗初始化与喂狗测试...\n");
    memset(&g_test_wdt, 0, sizeof(g_test_wdt));
    memset(&g_test_clk, 0, sizeof(g_test_clk));

    /* 模拟 LIRC 时钟稳定 */
    g_test_clk.STATUS = NUC472_CLK_STATUS_LIRCSTB;

    int ret = nuc472_wdt_init();
    assert(ret == 0);
    assert((g_test_clk.PWRCTL & NUC472_CLK_PWRCTL_LIRCEN) != 0UL);
    assert((g_test_clk.APBCLK0 & NUC472_CLK_APBCLK0_WDTCKEN) != 0UL);
    assert((g_test_clk.CLKSEL1 & NUC472_CLK_CLKSEL1_WDTSEL_Msk) == NUC472_CLK_CLKSEL1_WDTSEL_LIRC);
    /* 默认初始化必须保持看门狗关闭 */
    assert(g_test_wdt.CTL == 0UL);

    /* 开启看门狗 (2^14 LIRC 周期，使能复位) */
    ret = nuc472_wdt_start(5U, true);
    assert(ret == 0);
    assert((g_test_wdt.CTL & NUC472_WDT_CTL_WDTEN) != 0UL);
    assert((g_test_wdt.CTL & NUC472_WDT_CTL_RSTEN) != 0UL);
    assert(((g_test_wdt.CTL & NUC472_WDT_CTL_TOUTSEL_Msk) >> NUC472_WDT_CTL_TOUTSEL_Pos) == 5U);

    /* 模拟喂狗 */
    g_test_wdt.CTL &= ~NUC472_WDT_CTL_RSTCNT;
    ret = nuc472_wdt_feed();
    assert(ret == 0);
    assert((g_test_wdt.CTL & NUC472_WDT_CTL_RSTCNT) != 0UL);
    assert((g_test_wdt.CTL & NUC472_WDT_CTL_RSTEN) != 0UL);
    assert((g_test_wdt.CTL & NUC472_WDT_CTL_WDTEN) != 0UL);
    assert((g_test_wdt.CTL & NUC472_WDT_CTL_W1C_Msk) == 0UL);

    /* 停止看门狗 */
    ret = nuc472_wdt_stop();
    assert(ret == 0);
    assert(g_test_wdt.CTL == 0UL);

    printf("  -> WDT 测试通过!\n");
}

/* =========================================================================
 * 6. EMAC MDIO PHY 链路探测测试
 * ========================================================================= */
static void test_emac_phy_probe_periph(void)
{
    printf("[TEST] EMAC MDIO PHY (IP101GR) 探测测试...\n");
    memset(&g_test_emac, 0, sizeof(g_test_emac));
    memset(&g_test_clk, 0, sizeof(g_test_clk));

    int ret = nuc472_emac_phy_init();
    assert(ret == 0);
    assert((g_test_clk.AHBCLK & NUC472_CLK_AHBCLK_EMACCKEN) != 0UL);

    /* 校验 MDC 分频比 CLKDIV3 中的 EMACDIV: 84 MHz / (127 + 1) = 656.25 kHz <= 2.5 MHz */
    uint32_t emac_div = (g_test_clk.CLKDIV3 & NUC472_CLK_CLKDIV3_EMACDIV_Msk) >> NUC472_CLK_CLKDIV3_EMACDIV_Pos;
    assert(emac_div >= 33U);
    assert(emac_div == 127U);

    /* 校验 PB.14 (MDC) 与 PB.15 (MDIO) 引脚多功能复用为 EMAC (功能号 6) */
    assert((g_test_sys.GPB_MFPH & NUC472_SYS_GPB_MFPH_RMII_MDIO_Msk) == NUC472_SYS_GPB_MFPH_RMII_MDIO_Val);

    /* 模拟 IP101GR 在 PHYADDR=1 上的 MDIO 交互 */
    /* 6.1 模拟无 PHY 浮空总线 (返回 0xFFFF) 必须返回 -ENODEV 且 link_up=false */
    g_test_emac.MIIMDAT = 0xFFFFU;
    struct nuc472_phy_info no_phy_info;
    memset(&no_phy_info, 0, sizeof(no_phy_info));
    ret = nuc472_emac_phy_probe(0U, &no_phy_info);
    assert(ret == -ENODEV);
    assert(no_phy_info.link_up == false);

    /* 6.2 正常探测并验证 BMSR 连续读取两次消抖 Latch Low */
    struct nuc472_phy_info info;
    memset(&info, 0, sizeof(info));

    /* 驱动调用 mdio_read 时，在主机测试环境中我们拦截或通过回调提供值 */
    ret = nuc472_emac_phy_probe(1U, &info);
    assert(ret == 0);
    assert(info.phy_addr == 1U);
    assert(info.phy_id1 == 0x0243U);
    assert((info.phy_id2 & 0xFFF0U) == 0x0C50U);
    assert(info.is_ip101gr == true);
    assert(info.link_up == true);

    /* 6.2 模拟超时测试 (未响应导致 ETIMEDOUT) */
    uint16_t dummy_val = 0;
    ret = nuc472_emac_mdio_read_timeout_sim(1U, 2U, &dummy_val);
    assert(ret == -ETIMEDOUT);

    printf("  -> EMAC PHY Probe 测试通过!\n");
}

int main(void)
{
    printf("===== NUC472 片上外设驱动主机回归测试 =====\n");
    test_eadc_periph();
    test_pwm0_periph();
    test_timer0_periph();
    test_rtc_periph();
    test_wdt_periph();
    test_emac_phy_probe_periph();
    printf("===== 全部外设回归测试通过! =====\n");
    return 0;
}
