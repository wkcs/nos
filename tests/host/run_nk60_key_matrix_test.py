#!/usr/bin/env python3
"""Verify NK60 key matrix driver has no blocking sleep symbols and uses direct BSRR writes."""
from pathlib import Path
import re
import shutil
import subprocess
import sys

root = Path(__file__).resolve().parents[2]
build_dir = root / "build/nk60-v2"
obj_file = build_dir / "board/CMakeFiles/nos_board.dir/nk60-v2/key_matrix.c.obj"

if not obj_file.exists():
    if not shutil.which("arm-none-eabi-gcc"):
        print("SKIPPED: cross-compiler not installed in host environment")
        sys.exit(0)
    if not shutil.which("cmake"):
        print("SKIPPED: cmake not installed in host environment")
        sys.exit(0)
    subprocess.run(["cmake", "--preset", "nk60-v2"], cwd=root, check=True)
    subprocess.run(["cmake", "--build", "--preset", "nk60-v2", "--target", "nos_board"], cwd=root, check=True)

if not obj_file.exists():
    print(f"FAILED: Object file {obj_file} could not be built.")
    sys.exit(1)

nm_bin = shutil.which("arm-none-eabi-nm")
objdump_bin = shutil.which("arm-none-eabi-objdump")

if not nm_bin or not objdump_bin:
    print("SKIPPED: cross-compiler not installed in host environment")
    sys.exit(0)

# 检查符号引用中绝不含有任何 usleep 或 cpu_delay_us
symbols = subprocess.check_output([nm_bin, "-u", str(obj_file)], text=True)
for forbidden in ["usleep", "cpu_delay_us", "asm_cpu_delay_us"]:
    if forbidden in symbols:
        print(f"FAILED: forbidden symbol '{forbidden}' found in {obj_file.name}:\n{symbols}")
        sys.exit(1)

# 检查反汇编中包含对 BSRR 寄存器偏移 (0x18 / 24) 的原子写操作以及自适应 RC 恢复逻辑
disasm = subprocess.check_output([objdump_bin, "-d", "-r", str(obj_file)], text=True)
assert "nk60_v2_key_scan_once" in disasm, "nk60_v2_key_scan_once symbol missing in disassembly"
assert re.search(r"str\w*\s+r\d+,\s*\[r\d+,\s*#(24|0x18)\]", disasm) is not None, \
    "BSRR write (offset 24 or 0x18) not found in disassembly"
scan_match = re.search(r"<nk60_v2_key_scan_once>:(.*?)(?:\n\n|\Z)", disasm, re.S)
assert scan_match is not None, "failed to extract nk60_v2_key_scan_once disassembly"
scan_asm = scan_match.group(1)
idr_reads = re.findall(r"ldr\w*\s+r\d+,\s*\[r\d+,\s*#(16|0x10)\]", scan_asm)
assert len(idr_reads) >= 2, f"expected >= 2 IDR reads (pre-scan + loop), got {len(idr_reads)}"
assert re.search(r"\bsubs\b.*\n\s*[0-9a-f]+:\s+[0-9a-f]+\s+bne", scan_asm) is not None, \
    "deterministic cycle delay loop (subs/bne) not found in nk60_v2_key_scan_once"

print("NK60 key matrix fast-scan regression checks passed")
