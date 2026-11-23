/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 运行时扁平设备树（FDT）只读扫描器
 * 用于在访问可选外设 MMIO 之前确认其确实由固件/QEMU 实例化，
 * 避免对未解码物理槽位读写触发同步外部中止（SEA）。
 */

#ifndef __ASM_ARM64_FDT_SCAN_H__
#define __ASM_ARM64_FDT_SCAN_H__

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ARM64_FDT_MAGIC         0xD00DFEEDU

/**
 * 在给定的 FDT blob 中按 compatible 查找第 index 个匹配节点的首个 reg 区间
 * （假定 #address-cells = 2、#size-cells = 2，与 QEMU virt / RK3588 一致）。
 * 返回 0 表示找到，-ENOENT 表示不存在，-EINVAL 表示 blob 非法。
 */
int arm64_fdt_blob_find_reg(const void *blob,
                            size_t blob_max,
                            const char *compatible,
                            uint32_t index,
                            uint64_t *out_base,
                            uint64_t *out_size);

/**
 * 在运行时引导 FDT（x0 传入的地址，缺省回退 QEMU 裸机约定的 DRAM 起始）中查找
 */
int arm64_fdt_find_reg(const char *compatible,
                       uint32_t index,
                       uint64_t *out_base,
                       uint64_t *out_size);

/**
 * 运行时引导 FDT 是否可用（魔数校验通过）
 */
bool arm64_fdt_available(void);

#endif /* __ASM_ARM64_FDT_SCAN_H__ */
