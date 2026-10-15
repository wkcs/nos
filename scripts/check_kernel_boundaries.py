#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Check direct kernel branch destinations."""
import argparse
import json
from pathlib import Path
import re
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--image", required=True, type=Path)
    parser.add_argument("--prefix", required=True, help="Cross-binutils prefix, such as arm-none-eabi-")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    symbols = {}
    for line in subprocess.check_output([args.prefix + "nm", "--defined-only", "--format=posix", str(args.image)], text=True).splitlines():
        fields = line.split()
        if len(fields) >= 3:
            try:
                symbols[fields[0]] = int(fields[2], 16)
            except ValueError:
                continue
    if "__kernel_start" not in symbols or "__kernel_ro_end" not in symbols:
        parser.error("the image must export its kernel executable boundaries")
    low, high = symbols["__kernel_start"], symbols["__kernel_ro_end"]
    listing = subprocess.check_output([args.prefix + "objdump", "-d", "--section=.text", str(args.image)], text=True)
    pattern = re.compile(r"\b(?:b(?:\.[a-z]+)?|b[a-z]{2}|bl|blx|jal|j|call)\s+([0-9a-f]+)\s+<([^>]+)>")
    indirect_comment = re.compile(r"\bjalr\b.*#\s*([0-9a-f]+)\s+<([^>]+)>")
    checked, violations = 0, []
    for line in listing.splitlines():
        address = re.match(r"\s*([0-9a-f]+):", line)
        if not address or not low <= int(address[1], 16) < high:
            continue
        target = pattern.search(line) or indirect_comment.search(line)
        if not target:
            continue
        checked += 1
        if not low <= int(target[1], 16) < high:
            violations.append(line.strip())
    report = {"image": str(args.image), "passed": checked > 0 and not violations,
              "direct_branches_checked": checked, "kernel_start": hex(low), "kernel_ro_end": hex(high),
              "violations": violations,
              "scope": "Direct branches and linker-resolved RISC-V calls; indirect call safety requires runtime validation."}
    if args.output:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report))
    return 0 if report["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
