#!/usr/bin/env python3
"""Compile and run the multi-policy scheduler & Per-CPU SMP runqueue host test."""

from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]


def write_host_asm_stubs(stub_root: Path) -> None:
    asm_dir = stub_root / "asm"
    asm_dir.mkdir(parents=True, exist_ok=True)
    wk_dir = stub_root / "wk"
    wk_dir.mkdir(parents=True, exist_ok=True)
    kern_dir = stub_root / "kernel"
    kern_dir.mkdir(parents=True, exist_ok=True)

    (stub_root / "autocfg.h").write_text(
        "/* Host test configuration flags provided via command line. */\n",
        encoding="utf-8",
    )
    (wk_dir / "compiler-clang.h").write_text(
        "/* Clang compatibility stub for host tests. */\n",
        encoding="utf-8",
    )
    (kern_dir / "section.h").write_text(
        "#ifndef __NOS_SECTION_H__\n"
        "#define __NOS_SECTION_H__\n"
        "#define __init\n"
        "#define __task\n"
        "#define __console\n"
        "#define __sys_init\n"
        "#endif\n",
        encoding="utf-8",
    )
    (asm_dir / "types.h").write_text(
        "#ifndef __HOST_ASM_TYPES_H__\n"
        "#define __HOST_ASM_TYPES_H__\n"
        "#include <stddef.h>\n"
        "#include <stdint.h>\n"
        "#include <limits.h>\n"
        "#define _PID_T\n"
        "typedef int8_t s8;\n"
        "typedef uint8_t u8;\n"
        "typedef int16_t s16;\n"
        "typedef uint16_t u16;\n"
        "typedef int32_t s32;\n"
        "typedef uint32_t u32;\n"
        "typedef int64_t s64;\n"
        "typedef uint64_t u64;\n"
        "typedef uintptr_t addr_t;\n"
        "#define U8_MAX  ((u8)~0U)\n"
        "#define U16_MAX ((u16)~0U)\n"
        "#define U32_MAX ((u32)~0U)\n"
        "#define U64_MAX ((u64)~0ULL)\n"
        "#endif\n",
        encoding="utf-8",
    )
    (asm_dir / "barrier.h").write_text(
        "#ifndef __HOST_ASM_BARRIER_H__\n"
        "#define __HOST_ASM_BARRIER_H__\n"
        "#define smp_mb()  __sync_synchronize()\n"
        "#define smp_rmb() __sync_synchronize()\n"
        "#define smp_wmb() __sync_synchronize()\n"
        "#endif\n",
        encoding="utf-8",
    )
    (asm_dir / "spinlock.h").write_text(
        "#ifndef __HOST_ASM_SPINLOCK_H__\n"
        "#define __HOST_ASM_SPINLOCK_H__\n"
        "#include <stdint.h>\n"
        "typedef union {\n"
        "    uint32_t slock;\n"
        "    struct { uint16_t owner, next; } tickets;\n"
        "} arch_spinlock_t;\n"
        "static inline void arch_spin_lock(arch_spinlock_t *lock) { lock->tickets.next++; }\n"
        "static inline void arch_spin_unlock(arch_spinlock_t *lock) { lock->tickets.owner++; }\n"
        "#endif\n",
        encoding="utf-8",
    )
    (asm_dir / "task.h").write_text(
        "#ifndef __HOST_ASM_TASK_H__\n"
        "#define __HOST_ASM_TASK_H__\n"
        "#endif\n",
        encoding="utf-8",
    )
    (asm_dir / "cpu.h").write_text(
        "#ifndef __HOST_ASM_CPU_H__\n"
        "#define __HOST_ASM_CPU_H__\n"
        "#define USE_CPU_FFS\n"
        "static inline int __ffs(int value) { return __builtin_ffs(value); }\n"
        "void context_switch(addr_t from, addr_t to);\n"
        "void context_switch_to(addr_t to);\n"
        "#endif\n",
        encoding="utf-8",
    )
    (asm_dir / "init.h").write_text(
        "#ifndef __HOST_ASM_INIT_H__\n"
        "#define __HOST_ASM_INIT_H__\n"
        "#endif\n",
        encoding="utf-8",
    )


def main() -> None:
    with tempfile.TemporaryDirectory(prefix="nos-sched-multi-") as tmp:
        tmp_path = Path(tmp)
        stub_dir = tmp_path / "stubs"
        write_host_asm_stubs(stub_dir)
        exe = tmp_path / "test_sched_multi_policy"

        cmd = [
            os.environ.get("CC", "cc"),
            "-std=gnu11",
            "-O2",
            "-Wall",
            "-Wextra",
            "-D_FORTIFY_SOURCE=0",
            "-DCONFIG_SMP=1",
            "-DCONFIG_NR_CPUS=4",
            "-DCONFIG_SCHED_FAIR=1",
            "-DCONFIG_MAX_PRIORITY=32",
            "-DCONFIG_SYS_TICK_MS=1",
            "-I", str(stub_dir),
            "-I", str(ROOT / "include"),
            str(ROOT / "kernel/sched/sched_rt.c"),
            str(ROOT / "kernel/sched/sched_fair.c"),
            str(ROOT / "kernel/sched/sched_idle.c"),
            str(ROOT / "kernel/sched/sched_load.c"),
            str(ROOT / "kernel/sched/sched.c"),
            str(ROOT / "tests/host/test_sched_multi_policy.c"),
            "-o", str(exe),
        ]
        subprocess.run(cmd, check=True)
        subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == "__main__":
    main()
