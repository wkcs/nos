/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * NUC472 片上功能外设与 CON1 PHY 探测标准驱动接口
 * 提供给内核服务、hwtest、hwinfo 及 Lua 脚本调用
 */
#ifndef __NOS_DRIVERS_NUC472_PERIPHS_H__
#define __NOS_DRIVERS_NUC472_PERIPHS_H__

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* =========================================================================
 * 1. EADC 内部带隙基准 (VBG) 与温度传感器 (VTEMP) 驱动接口
 * ========================================================================= */

/**
 * @brief 初始化 NUC472 EADC 采样模块
 *        开启 APBCLK1 EADC 时钟、配置 AVDD 基准、使能内部温度传感器与 EADC 核心
 * @return 0 成功，负数表示错误码
 */
int nuc472_eadc_init(void);

/**
 * @brief 单次采样内部带隙基准 VBG 并换算为毫伏 (mV)
 * @param[out] mv 输出基准电压毫伏值 (典型值约 1200 mV)
 * @return 0 成功，-ETIMEDOUT 采样超时，-EINVAL 参数非法
 */
int nuc472_eadc_read_vbg_mv(uint32_t *mv);

/**
 * @brief 单次采样内部温度传感器 VTEMP 并换算为摄氏温度 (°C)
 * @param[out] temp_c 输出当前片上晶圆温度 (摄氏度)
 * @return 0 成功，-ETIMEDOUT 采样超时，-EINVAL 参数非法
 */
int nuc472_eadc_read_temp_celsius(int32_t *temp_c);

/**
 * @brief 单次软件触发指定通道采样并获取 12 位原始值 (0 ~ 4095)
 * @param[in]  ch 采样通道编号 (如通道 8=VBG, 9=VTEMP)
 * @param[out] raw_val 输出原始采样数值
 * @return 0 成功，-ETIMEDOUT 采样超时，-EINVAL 参数非法
 */
int nuc472_eadc_sample_raw(uint32_t ch, uint32_t *raw_val);


/* =========================================================================
 * 2. PWM0 波形输出驱动接口 (PA.5/PA.6)
 * ========================================================================= */

/**
 * @brief 初始化指定 PWM0 通道并配置对应 GPIO 引脚复用
 * @param[in] ch PWM 通道号 (0 ~ 5，板载 PA.5 对应通道 0，PA.6 对应通道 1)
 * @return 0 成功，-EINVAL 参数非法
 */
int nuc472_pwm_init(uint32_t ch);

/**
 * @brief 配置指定 PWM0 通道的工作频率与占空比并启动计数器与输出
 * @param[in] ch PWM 通道号 (0 ~ 5)
 * @param[in] freq_hz 目标波形频率 (Hz，必须大于 0)
 * @param[in] duty_percent 占空比百分比 (0 ~ 100)
 * @return 0 成功，-EINVAL 参数非法，-ETIMEDOUT 硬件稳定超时
 */
int nuc472_pwm_config(uint32_t ch, uint32_t freq_hz, uint32_t duty_percent);

/**
 * @brief 启动指定 PWM 通道波形输出
 * @param[in] ch PWM 通道号 (0 ~ 5)
 * @return 0 成功，-EINVAL 参数非法
 */
int nuc472_pwm_start(uint32_t ch);

/**
 * @brief 停止指定 PWM 通道波形输出
 * @param[in] ch PWM 通道号 (0 ~ 5)
 * @return 0 成功，-EINVAL 参数非法
 */
int nuc472_pwm_stop(uint32_t ch);


/* =========================================================================
 * 3. TIMER0 高精度周期定时器与滴答接口
 * ========================================================================= */

/**
 * @brief 初始化 TIMER0 为周期中断模式
 * @param[in] freq_hz 定时器中断触发频率 (Hz，必须大于 0)
 * @return 0 成功，-EINVAL 参数非法
 */
int nuc472_timer0_init(uint32_t freq_hz);

/**
 * @brief 启动 TIMER0 计数与中断
 * @return 0 成功
 */
int nuc472_timer0_start(void);

/**
 * @brief 停止 TIMER0 计数与中断
 * @return 0 成功
 */
int nuc472_timer0_stop(void);

/**
 * @brief 获取 TIMER0 累积的软件滴答中断计数
 * @return 64 位滴答计数值
 */
uint64_t nuc472_timer0_get_ticks(void);

/**
 * @brief 获取 TIMER0 微秒级时间戳 (由滴答与当前计数器合成)
 * @return 64 位微秒时间戳
 */
uint64_t nuc472_timer0_get_us(void);

/**
 * @brief TIMER0 硬件中断处理函数 (供中断向量表或 ISR 汇编入口转发)
 */
void nuc472_timer0_irq_handler(void);


/* =========================================================================
 * 4. RTC 实时日历时钟驱动接口
 * ========================================================================= */

/**
 * @brief 日期与时间结构体 (24 小时制)
 */
struct nuc472_rtc_time {
    uint32_t year;    /* 年份 (2000 ~ 2099) */
    uint32_t month;   /* 月份 (1 ~ 12) */
    uint32_t day;     /* 日期 (1 ~ 31) */
    uint32_t hour;    /* 小时 (0 ~ 23) */
    uint32_t minute;  /* 分钟 (0 ~ 59) */
    uint32_t second;  /* 秒钟 (0 ~ 59) */
    uint32_t weekday; /* 星期 (0: 星期日, 1 ~ 6: 星期一 ~ 星期六) */
};

/**
 * @brief 初始化 RTC 模块
 *        开启时钟、激活 32.768 kHz 域并解锁写保护
 *        注意：严格隔离并保护后备寄存器 SPR[0..1] (用于保存 boot_flag)
 * @return 0 成功，-ETIMEDOUT 硬件激活超时
 */
int nuc472_rtc_init(void);

/**
 * @brief 设置 RTC 日期与时间 (自动编码为 BCD 格式写入硬件)
 * @param[in] time 目标日期时间结构体指针
 * @return 0 成功，-EINVAL 参数非法，-ETIMEDOUT 写等待超时
 */
int nuc472_rtc_set_time(const struct nuc472_rtc_time *time);

/**
 * @brief 读取 RTC 当前日期与时间 (读取硬件并解码 BCD 格式)
 * @param[out] time 接收日期时间结构体指针
 * @return 0 成功，-EINVAL 参数非法
 */
int nuc472_rtc_get_time(struct nuc472_rtc_time *time);


/* =========================================================================
 * 5. WDT 看门狗定时器驱动接口 (基于片内 10 kHz LIRC)
 * ========================================================================= */

/* 看门狗超时周期选择 (对应 2^N 个 LIRC 时钟，每个时钟约 100 us) */
#define NUC472_WDT_TIMEOUT_2POW4   0U  /* 16 周期 ~ 1.6 ms */
#define NUC472_WDT_TIMEOUT_2POW6   1U  /* 64 周期 ~ 6.4 ms */
#define NUC472_WDT_TIMEOUT_2POW8   2U  /* 256 周期 ~ 25.6 ms */
#define NUC472_WDT_TIMEOUT_2POW10  3U  /* 1024 周期 ~ 102.4 ms */
#define NUC472_WDT_TIMEOUT_2POW12  4U  /* 4096 周期 ~ 409.6 ms */
#define NUC472_WDT_TIMEOUT_2POW14  5U  /* 16384 周期 ~ 1.6384 s */
#define NUC472_WDT_TIMEOUT_2POW16  6U  /* 65536 周期 ~ 6.5536 s */
#define NUC472_WDT_TIMEOUT_2POW18  7U  /* 262144 周期 ~ 26.2144 s */

/**
 * @brief 初始化 WDT 时钟与系统配置，默认保持看门狗关闭
 * @return 0 成功，-ETIMEDOUT LIRC 稳定超时
 */
int nuc472_wdt_init(void);

/**
 * @brief 开启看门狗计数
 * @param[in] timeout_interval 超时等级 (0 ~ 7，推荐 NUC472_WDT_TIMEOUT_2POW14 约 1.6 秒)
 * @param[in] enable_reset 超时后是否触发芯片硬件复位
 * @return 0 成功，-EINVAL 参数非法
 */
int nuc472_wdt_start(uint32_t timeout_interval, bool enable_reset);

/**
 * @brief 喂狗 (复位 18 位看门狗计数器)
 * @return 0 成功
 */
int nuc472_wdt_feed(void);

/**
 * @brief 停止看门狗计数
 * @return 0 成功
 */
int nuc472_wdt_stop(void);


/* =========================================================================
 * 6. EMAC MDIO PHY 链路探测驱动接口 (针对 CON1 上的 IP101GR PHY)
 * ========================================================================= */

/**
 * @brief PHY 探测信息结构体
 */
struct nuc472_phy_info {
    uint32_t phy_addr;   /* 探测到的 PHY 地址 (通常板载为 1) */
    uint16_t phy_id1;    /* PHY 标识符 1 (IP101GR 标称 0x0243) */
    uint16_t phy_id2;    /* PHY 标识符 2 (IP101GR 标称 0x0C54) */
    uint16_t bmsr;       /* 基本模式状态寄存器 BMSR 原值 */
    bool     link_up;    /* 以太网双绞线链路是否联通 (BMSR bit 2) */
    bool     is_ip101gr; /* 是否匹配 IP101GR 芯片 ID */
};

/**
 * @brief 初始化 EMAC MDIO 管理接口时钟与引脚
 * @return 0 成功
 */
int nuc472_emac_phy_init(void);

/**
 * @brief 通过 MDIO 读取指定 PHY 的 16 位寄存器
 * @param[in]  phy_addr PHY 硬件地址 (0 ~ 31)
 * @param[in]  reg_addr 寄存器地址 (0 ~ 31)
 * @param[out] val 读出的 16 位寄存器数据
 * @return 0 成功，-ETIMEDOUT 总线超时，-EINVAL 参数非法
 */
int nuc472_emac_mdio_read(uint32_t phy_addr, uint32_t reg_addr, uint16_t *val);

/**
 * @brief 通过 MDIO 写入指定 PHY 的 16 位寄存器
 * @param[in] phy_addr PHY 硬件地址 (0 ~ 31)
 * @param[in] reg_addr 寄存器地址 (0 ~ 31)
 * @param[in] val 写入的 16 位数据
 * @return 0 成功，-ETIMEDOUT 总线超时，-EINVAL 参数非法
 */
int nuc472_emac_mdio_write(uint32_t phy_addr, uint32_t reg_addr, uint16_t val);

/**
 * @brief 探测指定 PHY 地址并提取 ID 与链路状态
 * @param[in]  phy_addr PHY 地址 (板载 CON1 默认为 1)
 * @param[out] info 输出 PHY 探测详情
 * @return 0 成功，-ETIMEDOUT 探测超时/无响应，-EINVAL 参数非法
 */
int nuc472_emac_phy_probe(uint32_t phy_addr, struct nuc472_phy_info *info);

#ifdef __cplusplus
}
#endif

#endif /* __NOS_DRIVERS_NUC472_PERIPHS_H__ */
