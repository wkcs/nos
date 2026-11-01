#!/usr/bin/env python3
"""Verify keyboard profile operations fit in the NK60 task stacks."""
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
build = root / "build/nk60-v2"
commands = subprocess.check_output(["ninja", "-C", str(build), "-t", "commands"], text=True)
command = next(line for line in commands.splitlines()
               if "keyboard_server.c.obj" in line and " -c " in line)
args = shlex.split(command)
with tempfile.TemporaryDirectory(prefix="nos-keyboard-stack-") as tmp:
    output = Path(tmp) / "keyboard_server.o"
    for option, value in (("-o", str(output)), ("-MF", str(output) + ".d"),
                          ("-MT", str(output))):
        args[args.index(option) + 1] = value
    subprocess.run(args + ["-fstack-usage"], cwd=build, check=True)
    sizes = {}
    for line in output.with_suffix(".su").read_text().splitlines():
        location, size, _ = line.split("\t")
        sizes[location.rsplit(":", 1)[-1]] = int(size)
    for name in ("keyboard_config_save_profile", "keyboard_config_load_profile"):
        assert sizes[name] < 1024, f"{name} uses {sizes[name]} bytes of stack"
    print("keyboard profile stack checks passed", sizes["keyboard_config_save_profile"],
          sizes["keyboard_config_load_profile"])
