/**
 * Copyright (C) 2023-2026 Nick Hu <huqihan@live.com>
 *
 * ARM PSCI 0.2/1.0 电源与多核唤醒接口（HVC / SMC 统一封装）
 */

#ifndef __ASM_ARM64_PSCI_H__
#define __ASM_ARM64_PSCI_H__

#include <stdint.h>
#include <stdbool.h>

#define PSCI_FN_VERSION         0x84000000U
#define PSCI_FN_CPU_OFF         0x84000002U
#define PSCI_FN64_CPU_ON        0xC4000003U
#define PSCI_FN_SYSTEM_OFF      0x84000008U
#define PSCI_FN_SYSTEM_RESET    0x84000009U

#define PSCI_RET_SUCCESS        0L
#define PSCI_RET_NOT_SUPPORTED  (-1L)

void arm64_psci_set_conduit_smc(bool use_smc);
int32_t psci_get_version(void);
int psci_cpu_on(uint64_t target_mpidr, uintptr_t entry_phys, uint64_t context_id);
void psci_system_reset(void);
void psci_system_off(void);

#endif /* __ASM_ARM64_PSCI_H__ */
