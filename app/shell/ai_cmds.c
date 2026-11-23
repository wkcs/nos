/**
 * Copyright (C) 2026 Nick Hu <huqihan@live.com>
 *
 * nShell AI 原生内核诊断与 EL0 ELF64 启动扩展命令 (ai / runelf)
 */

#include <nos/shell_cmd.h>
#include <nos/accel.h>
#include <nos/elf64.h>
#include <nos/iommu.h>
#include <nos/object.h>
#include <nos/sched_infer.h>
#include <uapi/nos/errno.h>
#include <kernel/init.h>
#include <string.h>

#ifdef CONFIG_ARCH_ARM64
#include <asm/cpufeature.h>
#include <asm/security.h>
#include <asm/task.h>
#endif

static void ai_print_topo(void)
{
    struct nos_cpu_topo_info topo;

    memset(&topo, 0, sizeof(topo));
    if (sched_infer_query_topo(&topo) != 0) {
        nos_shell_error("CPU Topo: unavailable\r\n");
        return;
    }

    nos_shell_printf("CPU Topo: nr=%u online=0x%x big=0x%x little=0x%x isa=0x%x gangs=%u\r\n",
                     (unsigned int)topo.nr_cpus,
                     (unsigned int)topo.online_mask,
                     (unsigned int)topo.big_cpu_mask,
                     (unsigned int)topo.little_cpu_mask,
                     (unsigned int)topo.ai_isa_flags,
                     (unsigned int)topo.active_gangs);
}

static void ai_print_security(void)
{
#ifdef CONFIG_ARCH_ARM64
    struct arm64_sec_status sec;
    const struct arm64_cpu_caps *caps = arm64_get_cpu_caps();

    arm64_security_get_status(&sec);
    nos_shell_printf("Security: PAC=%s BTI=%s MTE=%s(v%u) PAN=%s AFDBM=%s SVE=%s(vl=%uB) CCA=%s\r\n",
                     sec.pac_enabled ? "on" : "off",
                     sec.bti_enabled ? "on" : "off",
                     sec.mte_enabled ? "on" : "off",
                     (unsigned int)sec.mte_version,
                     sec.pan_enabled ? "on" : "off",
                     sec.afdbm_enabled ? "on" : "off",
                     (caps != NULL && caps->has_sve) ? "on" : "off",
                     (unsigned int)arm64_sve_get_vl(),
                     sec.cca_realm_present ? "realm" : "none");
#else
    nos_shell_printf("Security: arch non-arm64\r\n");
#endif
}

static void ai_print_iommu(void)
{
    struct nos_iommu_info info;

    memset(&info, 0, sizeof(info));
    if (nos_iommu_get_status(&info) != 0) {
        nos_shell_error("IOMMU: query failed\r\n");
        return;
    }

    nos_shell_printf("IOMMU: mode=%s rev=0x%x sid=%u-bit pasids=%u bindings=%u cmdq=%lu\r\n",
                     info.hw_present ? "smmuv3-hw" : "shadow",
                     (unsigned int)info.arch_rev,
                     (unsigned int)info.sid_bits,
                     (unsigned int)info.max_pasids,
                     (unsigned int)info.active_bindings,
                     (unsigned long)info.cmdq_submitted);
}

static void ai_print_accel(void)
{
    static const uint32_t types[2] = { NOS_ACCEL_TYPE_VIRTIO, NOS_ACCEL_TYPE_RKNPU };
    uint32_t found = 0U;

    for (uint32_t i = 0U; i < 2U; i++) {
        struct nos_accel_device *dev = nos_accel_find_by_type(types[i]);
        if (dev != NULL) {
            struct nos_accel_caps caps;
            if (nos_accel_query_caps(dev, &caps) == 0) {
                nos_shell_printf("AccelQueue: dev=%s type=%u cores=%u sid=%u peak=%lu.%02lu TOPS done=%lu fail=%lu\r\n",
                                 caps.name,
                                 (unsigned int)caps.accel_type,
                                 (unsigned int)caps.core_count,
                                 (unsigned int)caps.stream_id,
                                 (unsigned long)(caps.peak_tops_x100 / 100ULL),
                                 (unsigned long)(caps.peak_tops_x100 % 100ULL),
                                 (unsigned long)dev->jobs_completed,
                                 (unsigned long)dev->jobs_failed);
                found++;
            }
            (void)nos_object_put(&dev->header);
        }
    }

    if (found == 0U) {
        nos_shell_printf("AccelQueue: none registered\r\n");
    }
}

static int cmd_ai(int argc, char *argv[])
{
    const char *sub = (argc >= 2) ? argv[1] : "all";

    if (strcmp(sub, "topo") == 0) {
        ai_print_topo();
    } else if (strcmp(sub, "sec") == 0) {
        ai_print_security();
    } else if (strcmp(sub, "iommu") == 0) {
        ai_print_iommu();
    } else if (strcmp(sub, "accel") == 0) {
        ai_print_accel();
    } else if (strcmp(sub, "all") == 0 || strcmp(sub, "info") == 0) {
        ai_print_topo();
        ai_print_security();
        ai_print_iommu();
        ai_print_accel();
    } else {
        nos_shell_error("Usage: ai [info|topo|sec|iommu|accel]\r\n");
        return -EINVAL;
    }

    return 0;
}

static int cmd_runelf(int argc, char *argv[])
{
    int exit_code = -1;
    struct nos_cell_telemetry tm;
    int rc;

    if (argc < 2) {
        nos_shell_error("Usage: runelf <elf_path> [args...]\r\n");
        return -EINVAL;
    }

    memset(&tm, 0, sizeof(tm));
    rc = nos_elf64_spawn_from_vfs_argv(argv[1],
                                       argc - 1,
                                       (const char *const *)&argv[1],
                                       NULL,
                                       &exit_code,
                                       &tm);
    if (rc != 0) {
        nos_shell_error("runelf: failed to run %s (err=%d)\r\n", argv[1], rc);
        return rc;
    }

    nos_shell_printf("runelf: %s exit=%d accel_ops=%lu tool_calls=%lu audit=%u\r\n",
                     argv[1],
                     exit_code,
                     (unsigned long)tm.accel_ops_used,
                     (unsigned long)tm.tool_calls_used,
                     (unsigned int)tm.audit_seq);
    return exit_code;
}

static struct nos_shell_cmd g_ai_shell_cmd = {
    .name = "ai",
    .desc = "Inspect AI CPU topo, ARMv9 security, SMMUv3 & AccelQueue",
    .func = cmd_ai,
};

static struct nos_shell_cmd g_runelf_shell_cmd = {
    .name = "runelf",
    .desc = "Spawn an AArch64 ELF64 binary inside an isolated Agent Cell",
    .func = cmd_runelf,
};

static int nshell_ai_cmds_init(void)
{
    (void)nos_shell_register_cmd(&g_ai_shell_cmd);
    (void)nos_shell_register_cmd(&g_runelf_shell_cmd);
    return 0;
}

SYS_INIT(nshell_ai_cmds_init, INIT_LEVEL_APPLICATION);
