#!/usr/bin/env python3
"""Exercise NOS kernel timer subsystem in both wheel mode and small-memory list mode."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]

for small_mem in (False, True):
    with tempfile.TemporaryDirectory(prefix="nos-timer-test-") as tmp:
        tmp_path = Path(tmp)
        stub_inc = tmp_path / "include"
        (stub_inc / "asm").mkdir(parents=True, exist_ok=True)
        (stub_inc / "wk").mkdir(parents=True, exist_ok=True)
        (stub_inc / "kernel").mkdir(parents=True, exist_ok=True)
        cfg = (
            "#define CONFIG_MAX_PRIORITY 32\n"
            "#define CONFIG_MAX_ORDER 9\n"
            "#define CONFIG_NR_CPUS 2\n"
            "#define CONFIG_SYS_TICK_MS 1\n"
        )
        if small_mem:
            cfg += "#define CONFIG_SMALL_MEMORY 1\n"
        (stub_inc / "autocfg.h").write_text(cfg, encoding="utf-8")
        (stub_inc / "wk" / "compiler-clang.h").write_text("/* stub */\n", encoding="utf-8")
        (stub_inc / "kernel" / "section.h").write_text(
            "#ifndef __NOS_SECTION_H__\n"
            "#define __NOS_SECTION_H__\n"
            "#define __init\n"
            "#define __task\n"
            "#define __console\n"
            "#define __sys_init\n"
            "#endif\n",
            encoding="utf-8",
        )
        (stub_inc / "asm" / "spinlock.h").write_text(
            "#ifndef TEST_ASM_SPINLOCK_H\n"
            "#define TEST_ASM_SPINLOCK_H\n"
            "#include <stdint.h>\n"
            "typedef struct { struct { uint16_t owner; uint16_t next; } tickets; } arch_spinlock_t;\n"
            "static inline void arch_spin_lock(arch_spinlock_t *l) { (void)l; }\n"
            "static inline void arch_spin_unlock(arch_spinlock_t *l) { (void)l; }\n"
            "#endif\n",
            encoding="utf-8",
        )

        exe = tmp_path / "test_timer_subsystem"
        subprocess.run(
            [
                os.environ.get("CC", "cc"),
                "-std=gnu11",
                "-O2",
                "-Wall",
                "-Wextra",
                "-D_SSIZE_T_DECLARED",
                "-D_FORTIFY_SOURCE=0",
                "-I",
                str(stub_inc),
                "-I",
                str(root / "arch/arm/include"),
                "-I",
                str(root / "include"),
                str(root / "kernel/timer.c"),
                str(root / "tests/host/test_timer_subsystem.c"),
                "-o",
                str(exe),
            ],
            check=True,
        )
        subprocess.run([str(exe)], check=True, timeout=15)
