/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 v8.3~v9 安全硬化子系统（PAC / BTI / MTE2 与 EL2 CCA/RMM 探测接口）
 */

#ifndef __ASM_ARM64_SECURITY_H__
#define __ASM_ARM64_SECURITY_H__

#include <asm/cpufeature.h>
#include <stdbool.h>
#include <stdint.h>

struct task_struct;

/* ARM64 每任务独立 128-bit 指针认证 APIA 密钥（防跨任务 ROP 伪造签名） */
struct arm64_pac_keys {
    uint64_t apia_lo;
    uint64_t apia_hi;
    bool     initialized;
};

struct arm64_sec_status {
    bool     pac_enabled;
    bool     bti_enabled;
    bool     mte_enabled;
    bool     pan_enabled;
    bool     afdbm_enabled;
    bool     cca_realm_present;
    uint8_t  mte_version;
    uint64_t sctlr_bits;
    uint64_t tcr_bits;
};

/**
 * 纯逻辑计算辅助：依据处理器能力掩码计算应置位的 SCTLR_EL1 安全控制位
 */
uint64_t arm64_security_compute_sctlr(uint64_t base_sctlr, const struct arm64_cpu_caps *caps);

/**
 * 基于 SplitMix64 纯整数混乱函数为任务生成非零 128-bit APIA 密钥
 */
void arm64_pac_generate_keys(struct arm64_pac_keys *out, uint64_t entropy);

/**
 * 在主核与每个 PSCI 从核早期启动阶段激活硬件安全扩展（严格按特性按需开启）
 */
void arm64_security_init_cpu(void);

/**
 * 任务切换时刷新当前核硬件 APIAKey 为 next 任务的独立密钥
 */
void arm64_security_on_context_switch(struct task_struct *next);

/**
 * 查询当前激活的安全策略位状态及 CCA/RMM 探测状态
 */
void arm64_security_get_status(struct arm64_sec_status *out);
void arm64_security_dump(void);

/**
 * EL2 ARM CCA (Realm Management Monitor) 版本探测预留框架
 */
bool arm64_cca_probe(void);

#endif /* __ASM_ARM64_SECURITY_H__ */
