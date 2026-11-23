/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * ARM64 运行时 FDT 只读扫描器实现（无动态内存、无递归、严格边界检查）
 */

#include <asm/fdt_scan.h>
#include <uapi/nos/errno.h>
#include <string.h>

#ifndef __HOST_TEST__
#include <asm/mmu.h>
#include <asm/platform.h>
#include <nos/devicetree.h>
#include <stdio.h>
#endif

#define FDT_BEGIN_NODE          0x1U
#define FDT_END_NODE            0x2U
#define FDT_PROP                0x3U
#define FDT_NOP                 0x4U
#define FDT_END                 0x9U

/* QEMU virt 对非 Linux 裸机 ELF 内核将 DTB 放在 DRAM 起始处 */
#define ARM64_FDT_FALLBACK_PA   0x40000000ULL
#define ARM64_FDT_MAX_SIZE      (2U * 1024U * 1024U)

static uint32_t fdt_be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static uint64_t fdt_be64(const uint8_t *p)
{
    return ((uint64_t)fdt_be32(p) << 32) | (uint64_t)fdt_be32(p + 4);
}

/* compatible 是以 '\0' 分隔的字符串列表，逐项精确比较 */
static bool fdt_compat_match(const uint8_t *val, uint32_t len, const char *want)
{
    size_t want_len = strlen(want);
    uint32_t pos = 0U;

    while (pos < len) {
        const char *s = (const char *)(val + pos);
        size_t slen = 0U;
        while (pos + slen < len && s[slen] != '\0') {
            slen++;
        }
        if (slen == want_len && memcmp(s, want, want_len) == 0) {
            return true;
        }
        pos += (uint32_t)slen + 1U;
    }
    return false;
}

int arm64_fdt_blob_find_reg(const void *blob,
                            size_t blob_max,
                            const char *compatible,
                            uint32_t index,
                            uint64_t *out_base,
                            uint64_t *out_size)
{
    const uint8_t *b = (const uint8_t *)blob;
    uint32_t total;
    uint32_t off_struct;
    uint32_t off_strings;
    uint32_t size_struct;
    uint32_t pos;
    uint32_t end;
    uint32_t hits = 0U;
    bool node_match = false;
    const uint8_t *node_reg = NULL;
    uint32_t node_reg_len = 0U;

    if (b == NULL || compatible == NULL || blob_max < 40U) {
        return -EINVAL;
    }
    if (fdt_be32(b) != ARM64_FDT_MAGIC) {
        return -EINVAL;
    }
    total = fdt_be32(b + 4);
    off_struct = fdt_be32(b + 8);
    off_strings = fdt_be32(b + 12);
    size_struct = fdt_be32(b + 36);
    if (total < 40U || total > blob_max || off_struct >= total ||
        off_strings >= total || size_struct > total - off_struct) {
        return -EINVAL;
    }

    pos = off_struct;
    end = off_struct + size_struct;
    while (pos + 4U <= end) {
        uint32_t tok = fdt_be32(b + pos);
        pos += 4U;

        switch (tok) {
        case FDT_BEGIN_NODE: {
            /* 跳过以 '\0' 结尾的节点名并 4 字节对齐 */
            while (pos < end && b[pos] != '\0') {
                pos++;
            }
            pos = (pos + 4U) & ~3U;
            node_match = false;
            node_reg = NULL;
            node_reg_len = 0U;
            break;
        }
        case FDT_END_NODE:
            if (node_match && node_reg != NULL && node_reg_len >= 16U) {
                if (hits == index) {
                    if (out_base != NULL) {
                        *out_base = fdt_be64(node_reg);
                    }
                    if (out_size != NULL) {
                        *out_size = fdt_be64(node_reg + 8);
                    }
                    return 0;
                }
                hits++;
            }
            node_match = false;
            node_reg = NULL;
            node_reg_len = 0U;
            break;
        case FDT_PROP: {
            uint32_t len;
            uint32_t nameoff;
            const char *name;

            if (pos + 8U > end) {
                return -EINVAL;
            }
            len = fdt_be32(b + pos);
            nameoff = fdt_be32(b + pos + 4U);
            pos += 8U;
            if (len > end - pos || nameoff >= total - off_strings) {
                return -EINVAL;
            }
            name = (const char *)(b + off_strings + nameoff);
            if (strcmp(name, "compatible") == 0 &&
                fdt_compat_match(b + pos, len, compatible)) {
                node_match = true;
            } else if (strcmp(name, "reg") == 0) {
                node_reg = b + pos;
                node_reg_len = len;
            }
            pos = (pos + len + 3U) & ~3U;
            break;
        }
        case FDT_NOP:
            break;
        case FDT_END:
            return -ENOENT;
        default:
            return -EINVAL;
        }
    }
    return -ENOENT;
}

int arm64_fdt_blob_scan_cpus(const void *blob,
                             size_t blob_max,
                             uint32_t *out_caps,
                             uint32_t max_cpus,
                             uint32_t *out_count)
{
    const uint8_t *b = (const uint8_t *)blob;
    uint32_t total;
    uint32_t off_struct;
    uint32_t off_strings;
    uint32_t size_struct;
    uint32_t pos;
    uint32_t end;
    uint32_t depth = 0U;
    bool in_cpus = false;
    bool in_cpu = false;
    uint32_t cpu_depth = 0U;
    bool is_cpu_name = false;
    bool is_cpu_type = false;
    bool has_reg = false;
    uint64_t cpu_reg = 0ULL;
    uint32_t cpu_cap = 0U;
    uint32_t found = 0U;
    uint32_t max_slot = 0U;

    if (b == NULL || out_caps == NULL || max_cpus == 0U || blob_max < 40U) {
        return -EINVAL;
    }
    if (fdt_be32(b) != ARM64_FDT_MAGIC) {
        return -EINVAL;
    }
    total = fdt_be32(b + 4);
    off_struct = fdt_be32(b + 8);
    off_strings = fdt_be32(b + 12);
    size_struct = fdt_be32(b + 36);
    if (total < 40U || total > blob_max || off_struct >= total ||
        off_strings >= total || size_struct > total - off_struct) {
        return -EINVAL;
    }

    memset(out_caps, 0, sizeof(uint32_t) * max_cpus);
    pos = off_struct;
    end = off_struct + size_struct;

    while (pos + 4U <= end) {
        uint32_t tok = fdt_be32(b + pos);
        pos += 4U;

        switch (tok) {
        case FDT_BEGIN_NODE: {
            const char *nname = (const char *)(b + pos);
            uint32_t nlen = 0U;

            while (pos + nlen < end && b[pos + nlen] != '\0') {
                nlen++;
            }
            pos = (pos + nlen + 4U) & ~3U;
            depth++;
            if (depth == 2U && nlen == 4U && memcmp(nname, "cpus", 4U) == 0) {
                in_cpus = true;
            } else if (in_cpus && depth == 3U && nlen >= 4U &&
                       memcmp(nname, "cpu@", 4U) == 0) {
                in_cpu = true;
                cpu_depth = depth;
                is_cpu_name = true;
                is_cpu_type = false;
                has_reg = false;
                cpu_reg = 0ULL;
                cpu_cap = 0U;
            } else if (in_cpus && depth == 3U) {
                in_cpu = true;
                cpu_depth = depth;
                is_cpu_name = false;
                is_cpu_type = false;
                has_reg = false;
                cpu_reg = 0ULL;
                cpu_cap = 0U;
            }
            break;
        }
        case FDT_END_NODE:
            if (in_cpu && depth == cpu_depth) {
                if ((is_cpu_name || is_cpu_type) && has_reg) {
                    uint32_t idx;
                    if ((cpu_reg & 0xFFULL) != 0ULL || found == 0U) {
                        idx = (uint32_t)(cpu_reg & 0xFFULL);
                    } else {
                        idx = (uint32_t)((cpu_reg >> 8) & 0xFFULL);
                    }
                    if (idx < max_cpus) {
                        out_caps[idx] = (cpu_cap != 0U) ? cpu_cap : 1024U;
                        if (idx + 1U > max_slot) {
                            max_slot = idx + 1U;
                        }
                        found++;
                    }
                }
                in_cpu = false;
            } else if (in_cpus && depth == 2U) {
                in_cpus = false;
            }
            if (depth > 0U) {
                depth--;
            }
            break;
        case FDT_PROP: {
            uint32_t len;
            uint32_t nameoff;
            const char *name;

            if (pos + 8U > end) {
                return -EINVAL;
            }
            len = fdt_be32(b + pos);
            nameoff = fdt_be32(b + pos + 4U);
            pos += 8U;
            if (len > end - pos || nameoff >= total - off_strings) {
                return -EINVAL;
            }
            name = (const char *)(b + off_strings + nameoff);
            if (in_cpu && depth == cpu_depth) {
                if (strcmp(name, "device_type") == 0 && len >= 3U &&
                    memcmp(b + pos, "cpu", 3U) == 0) {
                    is_cpu_type = true;
                } else if (strcmp(name, "reg") == 0) {
                    if (len == 4U) {
                        cpu_reg = (uint64_t)fdt_be32(b + pos);
                        has_reg = true;
                    } else if (len >= 8U) {
                        cpu_reg = fdt_be64(b + pos);
                        has_reg = true;
                    }
                } else if (strcmp(name, "capacity-dmips-mhz") == 0 && len >= 4U) {
                    cpu_cap = fdt_be32(b + pos);
                }
            }
            pos = (pos + len + 3U) & ~3U;
            break;
        }
        case FDT_NOP:
            break;
        case FDT_END:
            goto done;
        default:
            return -EINVAL;
        }
    }

done:
    if (out_count != NULL) {
        *out_count = max_slot;
    }
    return (found > 0U) ? 0 : -ENOENT;
}

int arm64_fdt_blob_find_irq(const void *blob,
                            size_t blob_max,
                            const char *compatible,
                            uint32_t index,
                            uint32_t irq_idx,
                            uint32_t *out_intid)
{
    const uint8_t *b = (const uint8_t *)blob;
    uint32_t total;
    uint32_t off_struct;
    uint32_t off_strings;
    uint32_t size_struct;
    uint32_t pos;
    uint32_t end;
    uint32_t hits = 0U;
    bool node_match = false;
    const uint8_t *node_irqs = NULL;
    uint32_t node_irqs_len = 0U;

    if (b == NULL || compatible == NULL || out_intid == NULL || blob_max < 40U) {
        return -EINVAL;
    }
    if (fdt_be32(b) != ARM64_FDT_MAGIC) {
        return -EINVAL;
    }
    total = fdt_be32(b + 4);
    off_struct = fdt_be32(b + 8);
    off_strings = fdt_be32(b + 12);
    size_struct = fdt_be32(b + 36);
    if (total < 40U || total > blob_max || off_struct >= total ||
        off_strings >= total || size_struct > total - off_struct) {
        return -EINVAL;
    }

    pos = off_struct;
    end = off_struct + size_struct;
    while (pos + 4U <= end) {
        uint32_t tok = fdt_be32(b + pos);
        pos += 4U;

        switch (tok) {
        case FDT_BEGIN_NODE:
            while (pos < end && b[pos] != '\0') {
                pos++;
            }
            pos = (pos + 4U) & ~3U;
            node_match = false;
            node_irqs = NULL;
            node_irqs_len = 0U;
            break;
        case FDT_END_NODE:
            if (node_match && node_irqs != NULL && node_irqs_len >= (irq_idx + 1U) * 12U) {
                if (hits == index) {
                    const uint8_t *ent = node_irqs + irq_idx * 12U;
                    uint32_t itype = fdt_be32(ent);
                    uint32_t inum = fdt_be32(ent + 4U);
                    *out_intid = (itype == 0U) ? (inum + 32U)
                                               : ((itype == 1U) ? (inum + 16U) : inum);
                    return 0;
                }
                hits++;
            }
            node_match = false;
            node_irqs = NULL;
            node_irqs_len = 0U;
            break;
        case FDT_PROP: {
            uint32_t len;
            uint32_t nameoff;
            const char *name;

            if (pos + 8U > end) {
                return -EINVAL;
            }
            len = fdt_be32(b + pos);
            nameoff = fdt_be32(b + pos + 4U);
            pos += 8U;
            if (len > end - pos || nameoff >= total - off_strings) {
                return -EINVAL;
            }
            name = (const char *)(b + off_strings + nameoff);
            if (strcmp(name, "compatible") == 0 &&
                fdt_compat_match(b + pos, len, compatible)) {
                node_match = true;
            } else if (strcmp(name, "interrupts") == 0) {
                node_irqs = b + pos;
                node_irqs_len = len;
            }
            pos = (pos + len + 3U) & ~3U;
            break;
        }
        case FDT_NOP:
            break;
        case FDT_END:
            return -ENOENT;
        default:
            return -EINVAL;
        }
    }
    return -ENOENT;
}

#ifndef __HOST_TEST__
static const void *arm64_fdt_runtime_blob(void)
{
    uint64_t cands[2];
    uint32_t i;

    cands[0] = arm64_get_boot_dtb_phys();
    cands[1] = ARM64_FDT_FALLBACK_PA;
    for (i = 0U; i < 2U; i++) {
        const uint8_t *va;
        /* 仅接受落在 512MB 线性直映 DRAM 窗口内的地址 */
        if (cands[i] < 0x40000000ULL || cands[i] >= 0x60000000ULL || (cands[i] & 7ULL) != 0ULL) {
            continue;
        }
        va = (const uint8_t *)arm64_phys_to_virt((uintptr_t)cands[i]);
        if (fdt_be32(va) == ARM64_FDT_MAGIC) {
            return va;
        }
    }
    return NULL;
}

bool arm64_fdt_available(void)
{
    return arm64_fdt_runtime_blob() != NULL;
}

int arm64_fdt_find_reg(const char *compatible,
                       uint32_t index,
                       uint64_t *out_base,
                       uint64_t *out_size)
{
    const void *blob = arm64_fdt_runtime_blob();
    if (blob == NULL) {
        return -ENODEV;
    }
    return arm64_fdt_blob_find_reg(blob, ARM64_FDT_MAX_SIZE, compatible, index,
                                   out_base, out_size);
}

int arm64_fdt_find_irq(const char *compatible,
                       uint32_t index,
                       uint32_t irq_idx,
                       uint32_t *out_intid)
{
    const void *blob = arm64_fdt_runtime_blob();
    if (blob == NULL) {
        return -ENODEV;
    }
    return arm64_fdt_blob_find_irq(blob, ARM64_FDT_MAX_SIZE, compatible, index,
                                   irq_idx, out_intid);
}

int arm64_fdt_scan_cpu_capacities(uint32_t *out_caps,
                                  uint32_t max_cpus,
                                  uint32_t *out_count)
{
    const void *blob;
    uint32_t dts_cnt = 0U;

    if (out_caps == NULL || max_cpus == 0U) {
        return -EINVAL;
    }

    blob = arm64_fdt_runtime_blob();
    if (blob != NULL &&
        arm64_fdt_blob_scan_cpus(blob, ARM64_FDT_MAX_SIZE, out_caps, max_cpus, out_count) == 0) {
        return 0;
    }

    memset(out_caps, 0, sizeof(uint32_t) * max_cpus);
    for (uint32_t i = 0U; i < max_cpus; i++) {
        char path[32];
        const struct nos_dt_node *node;
        uint32_t cap = 0U;

        snprintf(path, sizeof(path), "/cpus/cpu@%u", (unsigned int)i);
        node = nos_dt_find_path(path);
        if (node != NULL && nos_dt_read_u32(node, "capacity-dmips-mhz", 0U, &cap) == 0) {
            out_caps[i] = cap;
            dts_cnt = i + 1U;
        }
    }
    if (dts_cnt > 0U) {
        if (out_count != NULL) {
            *out_count = dts_cnt;
        }
        return 0;
    }

    return -ENOENT;
}
#endif
