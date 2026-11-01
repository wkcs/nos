#!/usr/bin/env python3
"""Exercise kallsyms binary search, dual-mode stack unwinder, PELT/loadavg, and multi-view ps dump."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory(prefix="nos-sched-load-dump-") as tmp:
    tmp_path = Path(tmp)
    stub_inc = tmp_path / "include"
    (stub_inc / "asm").mkdir(parents=True, exist_ok=True)
    (stub_inc / "wk").mkdir(parents=True, exist_ok=True)
    (stub_inc / "autocfg.h").write_text(
        "#define CONFIG_MAX_PRIORITY 32\n"
        "#define CONFIG_NR_CPUS 1\n"
        "#define CONFIG_KALLSYMS 1\n"
        "#define CONFIG_STACK_BACKTRACE 1\n"
        "#define CONFIG_BACKTRACE_MAX_DEPTH 12\n",
        encoding="utf-8",
    )
    (stub_inc / "wk" / "compiler-clang.h").write_text(
        "/* Host clang stub */\n",
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

    srcs = [
        str(root / "kernel/kallsyms.c"),
        str(root / "kernel/task/task_dump.c"),
        str(root / "tests/host/test_sched_load_dump.c"),
    ]
    sched_load = root / "kernel/sched/sched_load.c"
    if sched_load.exists():
        srcs.insert(2, str(sched_load))

    exe = tmp_path / "test_sched_load_dump"
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
            *srcs,
            "-o",
            str(exe),
        ],
        check=True,
    )
    subprocess.run([str(exe)], check=True, timeout=15)
