/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 片内 FMC 块驱动与 RTC Spare Boot Flag 主机单元回归测试
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NUC472_HOST_TEST 1
#include <soc/nuc472_regs.h>
#include <soc/nuc472_periph.h>
#include <soc/nuc472_soc.h>
#include <kernel/boot.h>
#include <kernel/board.h>
#include <kernel/errno.h>
#include <kernel/mutex.h>
#include <kernel/irq.h>
#include <drivers/block.h>

/* 模拟 128 KiB APROM LittleFS 分区 (0x00060000 ~ 0x0007FFFF) */
#define TEST_LFS_BASE 0x00060000UL
#define TEST_LFS_SIZE 0x00020000UL

static uint8_t g_sim_aprom_lfs[TEST_LFS_SIZE];
static NUC472_SYS_Type g_test_sys;
static NUC472_CLK_Type g_test_clk;
static NUC472_FMC_Type g_test_fmc;
static NUC472_RTC_Type g_test_rtc;

static uint32_t g_last_erase_addr = 0;
static uint32_t g_erase_count = 0;
static uint32_t g_write_count = 0;
static bool g_sim_fail_trigger = false;

/* 模拟系统与寄存器桩 */
NUC472_SYS_Type *nuc472_test_sys(void) { return &g_test_sys; }
NUC472_CLK_Type *nuc472_test_clk(void) { return &g_test_clk; }
NUC472_FMC_Type *nuc472_test_fmc(void) { return &g_test_fmc; }
NUC472_RTC_Type *nuc472_test_rtc(void) { return &g_test_rtc; }

const void *nuc472_test_flash_ptr(uint32_t addr)
{
    if (addr < TEST_LFS_BASE || addr >= (TEST_LFS_BASE + TEST_LFS_SIZE)) {
        return NULL;
    }
    return (const void *)&g_sim_aprom_lfs[addr - TEST_LFS_BASE];
}

/* 模拟中断桩 */
addr_t disable_irq_save(void) { return 0; }
void enable_irq_save(addr_t flags) { (void)flags; }

/* 主机模拟 FMC ISP 硬件执行桩 */
int fmc_isp_trigger_ramfunc(void)
{
    if (g_sim_fail_trigger) {
        g_test_fmc.ISPCTL |= NUC472_FMC_ISPCTL_ISPFF;
        return -EIO;
    }

    uint32_t cmd = g_test_fmc.ISPCMD;
    uint32_t addr = g_test_fmc.ISPADDR;

    if (cmd == NUC472_FMC_ISPCMD_PAGE_ERASE) {
        if (addr < TEST_LFS_BASE || addr + NUC472_FMC_PAGE_SIZE > (TEST_LFS_BASE + TEST_LFS_SIZE)) {
            return -EIO;
        }
        memset(&g_sim_aprom_lfs[addr - TEST_LFS_BASE], 0xFF, NUC472_FMC_PAGE_SIZE);
        g_last_erase_addr = addr;
        g_erase_count++;
    } else if (cmd == NUC472_FMC_ISPCMD_PROGRAM) {
        if (addr < TEST_LFS_BASE || addr + 4U > (TEST_LFS_BASE + TEST_LFS_SIZE)) {
            return -EIO;
        }
        uint32_t val = g_test_fmc.ISPDAT;
        uint32_t *dest = (uint32_t *)&g_sim_aprom_lfs[addr - TEST_LFS_BASE];
        *dest = val;
        g_write_count++;
    } else if (cmd == NUC472_FMC_ISPCMD_READ_UID) {
        uint32_t idx = addr / 4U;
        g_test_fmc.ISPDAT = 0xAABB0000UL | idx;
    } else if (cmd == NUC472_FMC_ISPCMD_READ_CID) {
        g_test_fmc.ISPDAT = 0xDAUL;
    } else if (cmd == NUC472_FMC_ISPCMD_READ_PID) {
        g_test_fmc.ISPDAT = 0x0472UL;
    }

    return 0;
}

/* 包含实现文件以进行直接单元测试 */
#include "../../soc/nuc472/fmc_ll.c"
#include "../../drivers/flash/fmc_nuc472.c"
#include "../../kernel/boot.c"

/* board/nutiny-nuc472/init.c 中的 boot_flag 实现 */
int board_write_boot_flag(uint32_t flag);
uint32_t board_read_boot_flag(void);
#include "../../board/nutiny-nuc472/init.c"

int main(void)
{
    printf("=== 开始运行 NUC472 FMC 块驱动与 RTC Spare Boot Flag 主机回归测试 ===\n");

    /* 初始化模拟硬件状态 */
    memset(&g_test_sys, 0, sizeof(g_test_sys));
    memset(&g_test_clk, 0, sizeof(g_test_clk));
    memset(&g_test_fmc, 0, sizeof(g_test_fmc));
    memset(&g_test_rtc, 0, sizeof(g_test_rtc));
    memset(g_sim_aprom_lfs, 0xFF, sizeof(g_sim_aprom_lfs));
    g_test_sys.REGLCTL = 1U;

    /* -------------------------------------------------------------
     * 测试 1: 验证 fmc_nuc472 get_info 返回 sector_size=512, sector_count=256
     * ------------------------------------------------------------- */
    printf("[Test 1] 验证 fmc_nuc472 块驱动几何参数...\n");
    const struct device *dev = &__device_nuc472_lfs;
    assert(dev != NULL);
    const struct block_driver_api *api = (const struct block_driver_api *)dev->api;
    assert(api != NULL);
    assert(api->get_info != NULL);

    uint32_t sector_size = 0, sector_count = 0;
    int ret = api->get_info(dev, &sector_size, &sector_count);
    assert(ret == 0);
    assert(sector_size == 512U);
    assert(sector_count == 256U);
    printf("  -> get_info 验证通过 (512B * 256 扇区 = 128 KiB)!\n");

    /* -------------------------------------------------------------
     * 测试 2: 验证未按 2048B (4 扇区) 对齐擦除返回 -EINVAL，
     *         正确对齐擦除 (lba=4, count=4) 填 0xFF 且仅调用物理页 0x00060800
     * ------------------------------------------------------------- */
    printf("[Test 2] 验证擦除对齐与物理页操作...\n");
    /* 先将整片填脏数据 */
    memset(g_sim_aprom_lfs, 0x5A, sizeof(g_sim_aprom_lfs));
    g_erase_count = 0;
    g_last_erase_addr = 0;

    /* 未对齐 lba=1, count=4 */
    assert(api->erase(dev, 1, 4) == -EINVAL);
    /* 未对齐 lba=4, count=3 */
    assert(api->erase(dev, 4, 3) == -EINVAL);
    /* 越界 lba=256, count=4 */
    assert(api->erase(dev, 256, 4) == -EINVAL);
    /* 越界 lba=254, count=4 */
    assert(api->erase(dev, 254, 4) == -EINVAL);

    /* 正确对齐擦除: lba=4 (偏移 4 * 512 = 2048 = 0x800, 物理地址 0x00060800), count=4 (2048B) */
    ret = api->erase(dev, 4, 4);
    assert(ret == 0);
    assert(g_erase_count == 1);
    assert(g_last_erase_addr == (TEST_LFS_BASE + 0x800UL));

    /* 验证对应 2048 字节已擦除为 0xFF，前后区域仍为 0x5A */
    for (size_t i = 0; i < 0x800U; i++) {
        assert(g_sim_aprom_lfs[i] == 0x5AU);
    }
    for (size_t i = 0x800U; i < 0x1000U; i++) {
        assert(g_sim_aprom_lfs[i] == 0xFFU);
    }
    for (size_t i = 0x1000U; i < TEST_LFS_SIZE; i++) {
        assert(g_sim_aprom_lfs[i] == 0x5AU);
    }
    printf("  -> 对齐擦除与物理地址 0x%08X 验证通过!\n", (unsigned int)(TEST_LFS_BASE + 0x800UL));

    /* -------------------------------------------------------------
     * 测试 3: 验证 write 在已擦除扇区写入 512B 数据并读回一致；
     *         拒绝跨界 lba (256)；试图未擦除覆盖非 0xFF 字返回 -EIO
     * ------------------------------------------------------------- */
    printf("[Test 3] 验证扇区写入、回读与非擦除覆盖防护...\n");
    uint8_t write_buf[512];
    uint8_t read_buf[512];
    for (size_t i = 0; i < sizeof(write_buf); i++) {
        write_buf[i] = (uint8_t)(i ^ 0xA5);
    }

    /* 跨界写 lba=256 */
    assert(api->write(dev, 256, write_buf, 1) == -EINVAL);
    /* 跨界读 lba=256 */
    assert(api->read(dev, 256, read_buf, 1) == -EINVAL);

    /* 在已擦除扇区 lba=4 写入 512B */
    g_write_count = 0;
    ret = api->write(dev, 4, write_buf, 1);
    assert(ret == 0);
    assert(g_write_count == (512U / 4U));

    /* 读回比对 */
    memset(read_buf, 0, sizeof(read_buf));
    ret = api->read(dev, 4, read_buf, 1);
    assert(ret == 0);
    assert(memcmp(write_buf, read_buf, sizeof(write_buf)) == 0);

    /* 再次写入相同数据，应触发跳过相同字，不再调用写硬件 (g_write_count 不变) */
    uint32_t writes_before = g_write_count;
    ret = api->write(dev, 4, write_buf, 1);
    assert(ret == 0);
    assert(g_write_count == writes_before);

    /* 试图未擦除覆盖非 0xFF：构造新缓冲区将某些 0 改为 1 */
    uint8_t bad_buf[512];
    memcpy(bad_buf, write_buf, sizeof(bad_buf));
    bad_buf[0] = 0xFF; /* 将原本可能含有 0 的位改为 1 */
    ret = api->write(dev, 4, bad_buf, 1);
    assert(ret == -EIO);
    printf("  -> 扇区写入、回读与 0->1 非擦除检测 (-EIO) 验证通过!\n");

    /* -------------------------------------------------------------
     * 测试 4: 验证 RTC->SPR[0..1] 读写与 Magic/校验和
     * ------------------------------------------------------------- */
    printf("[Test 4] 验证 RTC Spare Boot Flag 持久化与校验安全降级...\n");
    /* 模拟 RTC 硬件上电就绪状态 */
    g_test_rtc.INIT = 1U;
    g_test_rtc.RWEN = NUC472_RTC_RWEN_RWENF_Msk;
    g_test_rtc.SPR[0] = 0U;
    g_test_rtc.SPR[1] = 0U;

    /* 冷上电 (SPR=0) 返回 BOOT_NORMAL */
    assert(board_read_boot_flag() == (uint32_t)BOOT_NORMAL);
    assert(read_boot_flag() == (uint32_t)BOOT_NORMAL);

    /* 写入 BOOT_BOOTLOADER */
    ret = write_boot_flag((uint32_t)BOOT_BOOTLOADER);
    assert(ret == 0);
    assert(g_test_rtc.SPR[0] == get_boot_flag_data(BOOT_BOOTLOADER));
    assert(g_test_rtc.SPR[1] == (g_test_rtc.SPR[0] ^ 0xA55A4E55UL));
    assert((g_test_rtc.SPRCTL & NUC472_RTC_SPRCTL_SPRRWEN) != 0U);

    /* 正常读回 */
    assert(board_read_boot_flag() == get_boot_flag_data(BOOT_BOOTLOADER));
    assert(read_boot_flag() == (uint32_t)BOOT_BOOTLOADER);

    /* 篡改 SPR[1] 校验和后读取，安全降级回 BOOT_NORMAL */
    g_test_rtc.SPR[1] ^= 0x00000001UL;
    assert(board_read_boot_flag() == (uint32_t)BOOT_NORMAL);
    assert(read_boot_flag() == (uint32_t)BOOT_NORMAL);

    /* 恢复 SPR[1]，但篡改 SPR[0] 高 16 位 Magic */
    g_test_rtc.SPR[1] ^= 0x00000001UL; /* 恢复校验和对应的值 */
    g_test_rtc.SPR[0] = 0x12340000UL | (uint32_t)BOOT_BOOTLOADER;
    g_test_rtc.SPR[1] = g_test_rtc.SPR[0] ^ 0xA55A4E55UL;
    assert(board_read_boot_flag() == (uint32_t)BOOT_NORMAL);
    assert(read_boot_flag() == (uint32_t)BOOT_NORMAL);
    printf("  -> RTC Spare Boot Flag 写入、校验与安全降级验证通过!\n");

    /* -------------------------------------------------------------
     * 测试 5: 验证 FMC 底层 uid / cid / pid 读取
     * ------------------------------------------------------------- */
    printf("[Test 5] 验证 FMC 底层 UID 与 Chip ID 读取...\n");
    uint32_t uid[3] = {0};
    ret = nuc472_fmc_read_uid(uid);
    assert(ret == 0);
    assert(uid[0] == 0xAABB0000UL);
    assert(uid[1] == 0xAABB0001UL);
    assert(uid[2] == 0xAABB0002UL);

    uint32_t cid = 0, pid = 0;
    ret = nuc472_fmc_read_id(&cid, &pid);
    assert(ret == 0);
    assert(cid == 0xDAUL);
    assert(pid == 0x0472UL);
    printf("  -> FMC 底层 ID 读取验证通过!\n");

    printf("=============================================================\n");
    printf("所有 NUC472 FMC 块驱动与 RTC Boot Flag 单元测试全部通过!\n");
    printf("=============================================================\n");
    return 0;
}
