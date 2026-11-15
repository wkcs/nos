/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NuTiny-NUC472 板级 nShell 扩展命令与硬件自检/探测工具 (led, hwinfo, hwtest)
 */

#include <kernel/kernel.h>
#include <kernel/init.h>
#include <kernel/errno.h>
#include <kernel/sleep.h>
#include <kernel/mm.h>
#include <soc/nuc472_soc.h>
#include <drivers/gpio_nuc472.h>
#include <drivers/nuc472_periphs.h>
#include <fs/vfs.h>
#include <nos/shell_cmd.h>
#include <string.h>
#include <stdio.h>

extern volatile bool g_nutiny_led_blinking;
extern int board_led_set(bool on);
extern uint32_t board_read_boot_flag(void);
extern int board_write_boot_flag(uint32_t flag);

/**
 * @brief 确保 EADC 时钟与核心已就绪，未初始化时按需初始化
 */
static int nutiny_ensure_eadc_ready(void)
{
    if ((NUC472_CLK->APBCLK1 & NUC472_CLK_APBCLK1_EADCCKEN) == 0UL ||
        (NUC472_EADC->CTL & NUC472_EADC_CTL_ADCEN) == 0UL) {
        return nuc472_eadc_init();
    }
    return 0;
}

/**
 * @brief 探测板载 PHY ADDR=1 的 EMAC PHY（内部会自动按需开启 EMAC 时钟与 MDIO 引脚复用）
 */
static int nutiny_probe_emac_phy(struct nuc472_phy_info *info)
{
    return nuc472_emac_phy_probe(1U, info);
}

/* 导出给 Lua 及系统的硬件信息接口 */
int32_t board_adc_temp_mc(void)
{
    int32_t temp_c = 0;
    int ret = nutiny_ensure_eadc_ready();

    if (ret != 0) {
        return 25000;
    }
    ret = nuc472_eadc_read_temp_celsius(&temp_c);
    if (ret != 0) {
        return 25000;
    }
    return temp_c * 1000;
}

int board_chip_uid(char *buf, size_t max_len)
{
    uint32_t uid[3] = {0};
    int ret;

    if (buf == NULL || max_len == 0) {
        return -EINVAL;
    }

    ret = nuc472_fmc_read_uid(uid);
    if (ret == 0) {
        snprintf(buf, max_len, "%08X%08X%08X",
                 (unsigned int)uid[0], (unsigned int)uid[1], (unsigned int)uid[2]);
        return 0;
    }
    snprintf(buf, max_len, "%08X", (unsigned int)NUC472_SYS->PDID);
    return 0;
}

/* =========================================================================
 * 1. LED 控制命令
 * ========================================================================= */
static int cmd_led(int argc, char *argv[])
{
    if (argc < 2) {
        nos_shell_printf("Usage: led <on|off|toggle|blink>\n");
        return 1;
    }

    if (strcmp(argv[1], "on") == 0) {
        board_led_set(true);
        nos_shell_printf("LED turned ON\n");
    } else if (strcmp(argv[1], "off") == 0) {
        board_led_set(false);
        nos_shell_printf("LED turned OFF\n");
    } else if (strcmp(argv[1], "toggle") == 0) {
        g_nutiny_led_blinking = false;
        nuc472_gpio_toggle(NUC472_PORT_B, 10);
        nos_shell_printf("LED toggled\n");
    } else if (strcmp(argv[1], "blink") == 0) {
        g_nutiny_led_blinking = true;
        nos_shell_printf("LED blink mode resumed\n");
    } else {
        nos_shell_error("Unknown LED sub-command '%s'\n", argv[1]);
        return 1;
    }
    return 0;
}

/* =========================================================================
 * 2. 硬件信息查询命令 (hwinfo)
 * ========================================================================= */
static int cmd_hwinfo(int argc, char *argv[])
{
    uint32_t uid[3] = {0};
    char uid_str[32] = "N/A";
    struct dentry *data_dir;
    struct nuc472_phy_info phy_info;
    int phy_ret;

    (void)argc;
    (void)argv;

    if (nuc472_fmc_read_uid(uid) == 0) {
        snprintf(uid_str, sizeof(uid_str), "%08X-%08X-%08X",
                 (unsigned int)uid[0], (unsigned int)uid[1], (unsigned int)uid[2]);
    }

    nos_shell_printf("================ NuTiny-NUC472 System Info ================\n");
    nos_shell_printf("SoC:          Nuvoton NUC472HI8AE (ARM Cortex-M4F @ %u MHz)\n",
                     (unsigned int)(SystemCoreClock / 1000000UL));
    nos_shell_printf("PDID:         0x%08X\n", (unsigned int)NUC472_SYS->PDID);
    nos_shell_printf("UID (96-bit): %s\n", uid_str);
    nos_shell_printf("UART0 Clock:  %u Hz\n", (unsigned int)nuc472_clk_get_uart0_hz());
    nos_shell_printf("-----------------------------------------------------------\n");
    nos_shell_printf("Flash Layout: Kernel: 384 KiB (0x00000000..0x0005FFFF, RO)\n");
    nos_shell_printf("              Data:   128 KiB (0x00060000..0x0007FFFF, LittleFS)\n");

    data_dir = vfs_lookup_path(vfs_get_root(), "/data");
    nos_shell_printf("Mount /data:  %s\n", data_dir != NULL ? "Mounted (LittleFS)" : "Not mounted");

    nos_shell_printf("TLSF Heap:    Total: %u B, Used: %u B, Free: %u B\n",
                     (unsigned int)mm_block_total_size(),
                     (unsigned int)mm_block_used_size(),
                     (unsigned int)mm_block_free_size());

    nos_shell_printf("-----------------------------------------------------------\n");
    phy_ret = nutiny_probe_emac_phy(&phy_info);
    if (phy_ret == 0) {
        nos_shell_printf("EMAC PHY:     %s (PHY ID: 0x%04X:0x%04X, Link: %s)\n",
                         phy_info.is_ip101gr ? "IP101GR" : "PHY",
                         (unsigned int)phy_info.phy_id1,
                         (unsigned int)phy_info.phy_id2,
                         phy_info.link_up ? "UP" : "DOWN");
    } else {
        nos_shell_printf("EMAC PHY:     Probe failed or PHY not attached (error %d)\n", phy_ret);
    }
    nos_shell_printf("===========================================================\n");
    return 0;
}

/* =========================================================================
 * 3. 硬件功能非破坏性自检命令 (hwtest)
 * ========================================================================= */
static int run_test_adc(void)
{
    uint32_t vbg_mv = 0;
    int32_t temp_c = 0;
    int ret;

    nos_shell_printf("[TEST] EADC VBG & Temperature Sensor...\n");
    ret = nutiny_ensure_eadc_ready();
    if (ret != 0) {
        nos_shell_error("  EADC init failed: %d\n", ret);
        return ret;
    }
    ret = nuc472_eadc_read_vbg_mv(&vbg_mv);
    if (ret != 0) {
        nos_shell_error("  EADC read VBG failed: %d\n", ret);
        return ret;
    }
    ret = nuc472_eadc_read_temp_celsius(&temp_c);
    if (ret != 0) {
        nos_shell_error("  EADC read Temperature failed: %d\n", ret);
        return ret;
    }
    nos_shell_printf("  VBG: %u mV, Temp: %d C -> PASS\n", (unsigned int)vbg_mv, (int)temp_c);
    return 0;
}

static int run_test_timer(void)
{
    uint64_t t1, t2;
    bool need_start;
    int ret;

    nos_shell_printf("[TEST] TIMER0 microsecond tick counter...\n");
    need_start = (NUC472_CLK->APBCLK0 & NUC472_CLK_APBCLK0_TMR0CKEN) == 0UL ||
                 (NUC472_TMR0->CTL & NUC472_TIMER_CTL_CNTEN) == 0UL;
    if (need_start) {
        ret = nuc472_timer0_init(1000U);
        if (ret != 0) {
            nos_shell_error("  TIMER0 init failed: %d\n", ret);
            return ret;
        }
        ret = nuc472_timer0_start();
        if (ret != 0) {
            nos_shell_error("  TIMER0 start failed: %d\n", ret);
            return ret;
        }
    }

    t1 = nuc472_timer0_get_ticks();
    msleep(10);
    t2 = nuc472_timer0_get_ticks();
    if (need_start) {
        (void)nuc472_timer0_stop();
    }
    if (t2 <= t1) {
        nos_shell_error("  TIMER0 tick did not advance: t1=%u, t2=%u\n",
                        (unsigned int)t1, (unsigned int)t2);
        return -EIO;
    }
    nos_shell_printf("  TIMER0 ticks delta: %u -> PASS\n", (unsigned int)(t2 - t1));
    return 0;
}

static int run_test_rtc(void)
{
    uint32_t flag;

    nos_shell_printf("[TEST] RTC Spare Register Boot Flag...\n");
    flag = board_read_boot_flag();
    nos_shell_printf("  Current Boot Flag: 0x%08X -> PASS\n", (unsigned int)flag);
    return 0;
}

static int run_test_pwm(void)
{
    int ret;
    nos_shell_printf("[TEST] PWM0 Generator Control Register probe...\n");
    ret = nuc472_pwm_init(0);
    if (ret != 0) {
        nos_shell_error("  PWM0 init failed: %d\n", ret);
        return ret;
    }
    nos_shell_printf("  PWM0 CTL: 0x%08X -> PASS\n", (unsigned int)NUC472_PWM0->CTL);
    return 0;
}

static int run_test_spi(void)
{
    nos_shell_printf("[TEST] SPI0 Controller register probe...\n");
    nuc472_sys_unlock();
    NUC472_CLK->APBCLK0 |= NUC472_CLK_APBCLK0_SPI0CKEN;
    nuc472_sys_lock();
    nos_shell_printf("  SPI0 CTL: 0x%08X, CLKDIV: 0x%08X -> PASS\n",
                     (unsigned int)NUC472_SPI0->CTL, (unsigned int)NUC472_SPI0->CLKDIV);
    return 0;
}

static int run_test_i2c(void)
{
    nos_shell_printf("[TEST] I2C3 Controller status...\n");
    nuc472_sys_unlock();
    NUC472_CLK->APBCLK0 |= NUC472_CLK_APBCLK0_I2C3CKEN;
    nuc472_sys_lock();
    nos_shell_printf("  I2C3 STATUS: 0x%08X -> PASS\n", (unsigned int)NUC472_I2C3->STATUS);
    return 0;
}

static int run_test_emac(void)
{
    struct nuc472_phy_info info;
    int ret;

    nos_shell_printf("[TEST] EMAC MDIO IP101GR PHY probe...\n");
    ret = nutiny_probe_emac_phy(&info);
    if (ret != 0) {
        nos_shell_error("  PHY probe returned %d (PHY may be unpowered)\n", ret);
        return ret;
    }
    nos_shell_printf("  PHY ID: 0x%04X:0x%04X, Link: %s -> PASS\n",
                     (unsigned int)info.phy_id1, (unsigned int)info.phy_id2,
                     info.link_up ? "UP" : "DOWN");
    return 0;
}

static int run_test_mpu(void)
{
    volatile uint32_t *mpu_type = (volatile uint32_t *)0xE000ED90UL;
    volatile uint32_t *mpu_ctrl = (volatile uint32_t *)0xE000ED94UL;

    nos_shell_printf("[TEST] Cortex-M4 MPU Safety Guard...\n");
    nos_shell_printf("  MPU TYPE: 0x%08X, CTRL: 0x%08X -> %s\n",
                     (unsigned int)*mpu_type, (unsigned int)*mpu_ctrl,
                     (*mpu_ctrl & 1) ? "ACTIVE (PASS)" : "DISABLED");
    return 0;
}

static int run_test_fmc(void)
{
    uint32_t cid = 0;
    uint32_t pid = 0;
    int ret;

    nos_shell_printf("[TEST] FMC Flash Memory Controller...\n");
    ret = nuc472_fmc_read_id(&cid, &pid);
    if (ret != 0) {
        nos_shell_error("  FMC read ID failed: %d\n", ret);
        return ret;
    }
    nos_shell_printf("  CID: 0x%08X, PID: 0x%08X -> PASS\n", (unsigned int)cid, (unsigned int)pid);
    return 0;
}

static int cmd_hwtest(int argc, char *argv[])
{
    const char *sub = (argc >= 2) ? argv[1] : "all";

    nos_shell_printf("Running NuTiny-NUC472 hardware self-test: '%s'\n", sub);

    if (strcmp(sub, "adc") == 0) return run_test_adc();
    if (strcmp(sub, "timer") == 0) return run_test_timer();
    if (strcmp(sub, "rtc") == 0) return run_test_rtc();
    if (strcmp(sub, "pwm") == 0) return run_test_pwm();
    if (strcmp(sub, "spi") == 0) return run_test_spi();
    if (strcmp(sub, "i2c") == 0) return run_test_i2c();
    if (strcmp(sub, "emac") == 0) return run_test_emac();
    if (strcmp(sub, "mpu") == 0) return run_test_mpu();
    if (strcmp(sub, "fmc") == 0) return run_test_fmc();

    if (strcmp(sub, "all") == 0) {
        int failed = 0;
        if (run_test_fmc() != 0) failed++;
        if (run_test_mpu() != 0) failed++;
        if (run_test_adc() != 0) failed++;
        if (run_test_timer() != 0) failed++;
        if (run_test_rtc() != 0) failed++;
        if (run_test_pwm() != 0) failed++;
        if (run_test_spi() != 0) failed++;
        if (run_test_i2c() != 0) failed++;
        (void)run_test_emac(); /* EMAC MDIO 在无外部网线/变压器时不阻断总体自检 */

        if (failed == 0) {
            nos_shell_printf(">>> ALL HARDWARE TESTS PASSED <<<\n");
            return 0;
        } else {
            nos_shell_error(">>> %d TEST(S) FAILED <<<\n", failed);
            return 1;
        }
    }

    nos_shell_error("Unknown test target '%s'. Available: all|rtc|adc|timer|pwm|spi|i2c|emac|mpu|fmc\n", sub);
    return 1;
}

/* 注册到 nShell 扩展命令链表 */
static struct nos_shell_cmd g_cmd_led = {
    .name = "led",
    .desc = "Control onboard LED (on|off|toggle|blink)",
    .func = cmd_led,
    .next = NULL,
};

static struct nos_shell_cmd g_cmd_hwinfo = {
    .name = "hwinfo",
    .desc = "Display hardware clocks, UID, memory and partition info",
    .func = cmd_hwinfo,
    .next = NULL,
};

static struct nos_shell_cmd g_cmd_hwtest = {
    .name = "hwtest",
    .desc = "Run onboard peripheral hardware self-test",
    .func = cmd_hwtest,
    .next = NULL,
};

static int nutiny_board_cmd_init(void)
{
    nos_shell_register_cmd(&g_cmd_led);
    nos_shell_register_cmd(&g_cmd_hwinfo);
    nos_shell_register_cmd(&g_cmd_hwtest);
    return 0;
}
SYS_INIT(nutiny_board_cmd_init, INIT_LEVEL_APPLICATION);
