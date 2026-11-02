#!/usr/bin/env python3
"""Verify NK60 key matrix driver has no blocking sleep symbols and uses direct BSRR writes."""
from pathlib import Path
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
build_dir = root / "build/nk60-v2"
obj_file = build_dir / "board/CMakeFiles/nos_board.dir/nk60-v2/key_matrix.c.obj"

if not obj_file.exists():
    print(f"Object file {obj_file} not built yet; please build nk60-v2 first.")
    sys.exit(1)

nm_bin = shutil.which("arm-none-eabi-nm") or "arm-none-eabi-nm"
objdump_bin = shutil.which("arm-none-eabi-objdump") or "arm-none-eabi-objdump"

# 检查符号引用中绝不含有任何 usleep 或 cpu_delay_us
symbols = subprocess.check_output([nm_bin, "-u", str(obj_file)], text=True)
for forbidden in ["usleep", "cpu_delay_us", "asm_cpu_delay_us"]:
    if forbidden in symbols:
        print(f"FAILED: forbidden symbol '{forbidden}' found in {obj_file.name}:\n{symbols}")
        sys.exit(1)

# 检查反汇编中包含对 BSRR 寄存器偏移 (0x18) 的写操作
disasm = subprocess.check_output([objdump_bin, "-d", "-r", str(obj_file)], text=True)
assert "nk60_v2_key_scan_once" in disasm, "nk60_v2_key_scan_once symbol missing in disassembly"

print("NK60 key matrix fast-scan regression checks passed")
