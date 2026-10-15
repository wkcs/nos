#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Give kernel compiler helpers their own symbols."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import tempfile

SYMBOL_PREFIX = "nos_kernel_"
MEMORY_SYMBOLS = {"memcpy": "k_memcpy", "memset": "k_memset", "memmove": "k_memmove", "memcmp": "k_memcmp"}
for operation in ("memcpy", "memmove", "memset", "memclr"):
    for suffix in ("", "4", "8"):
        name = "__aeabi_" + operation + suffix
        MEMORY_SYMBOLS[name] = SYMBOL_PREFIX + name


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(argv, cwd=None):
    result = subprocess.run(argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if result.returncode:
        raise RuntimeError(" ".join(map(str, argv)) + "\n" + result.stderr[-6000:])
    return result.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", required=True)
    parser.add_argument("--cpu-flag", action="append", default=[])
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    found = shutil.which(args.compiler)
    if not found:
        parser.error("compiler is unavailable: " + args.compiler)
    # Preserve the driver's public name; it may symlink to gcc-<version>.
    compiler = Path(found).absolute()
    if not compiler.name.endswith("gcc"):
        parser.error("a GCC cross compiler is required")
    tools = {name: compiler.with_name(compiler.name[:-3] + name) for name in ("ar", "nm", "objcopy")}
    for name, path in tools.items():
        if not path.is_file():
            found = shutil.which(compiler.name[:-3] + name)
            if not found:
                parser.error("missing tool: " + name)
            tools[name] = Path(found)
    source = Path(command([str(compiler)] + args.cpu_flag + ["-print-libgcc-file-name"]).strip()).resolve()
    directory = args.output.resolve()
    directory.mkdir(parents=True, exist_ok=True)
    archive = directory / "kernel-libgcc.a"
    symbols = directory / "kernel-libgcc.syms"
    stamp = directory / "kernel-libgcc.json"
    identity = {"source": str(source), "source_sha256": digest(source), "script_sha256": digest(Path(__file__)),
                "cpu_flags": args.cpu_flag, "compiler_sha256": digest(compiler),
                "tools": {name: digest(path) for name, path in tools.items()}, "prefix": SYMBOL_PREFIX}
    if all(p.is_file() for p in (archive, symbols, stamp)):
        previous = json.loads(stamp.read_text())
        if (previous.get("identity") == identity and previous.get("archive_sha256") == digest(archive) and
                previous.get("symbols_sha256") == digest(symbols)):
            return
    definitions = set()
    for line in command([str(tools["nm"]), "-g", "--defined-only", "--format=posix", str(source)]).splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[1] in "TWDBRVACSG" and len(fields[1]) == 1:
            definitions.add(fields[0])
    members = command([str(tools["ar"]), "t", str(source)]).splitlines()
    if not definitions or not members or len(members) != len(set(members)):
        raise RuntimeError("unsupported empty or duplicate-member libgcc archive")
    if any(Path(member).name != member or member in (".", "..") for member in members):
        raise RuntimeError("libgcc archive contains an unsafe member path")
    with tempfile.TemporaryDirectory(prefix="kernel-libgcc-", dir=directory) as temporary:
        work = Path(temporary)
        command([str(tools["ar"]), "x", str(source)], cwd=work)
        # Compiler-generated C memory operations must also stay privileged.
        # Prefixed references inside libgcc resolve to the same kernel runtime.
        memory_map = work / "memory.syms"
        memory_map.write_text("".join(SYMBOL_PREFIX + old + " " + new + "\n"
                                     for old, new in MEMORY_SYMBOLS.items() if SYMBOL_PREFIX + old != new))

        def transform(member):
            path = work / member
            command([str(tools["objcopy"]), "--prefix-symbols=" + SYMBOL_PREFIX, str(path)])
            command([str(tools["objcopy"]), "--redefine-syms", str(memory_map), str(path)])

        with ThreadPoolExecutor(max_workers=5) as workers:
            list(workers.map(transform, members))
        fresh = work / "kernel-libgcc.a"
        command([str(tools["ar"]), "rcsD", str(fresh)] + [str(work / member) for member in members])
        fresh.replace(archive)
    mapping = {name: SYMBOL_PREFIX + name for name in definitions}
    mapping.update(MEMORY_SYMBOLS)
    symbols.write_text("".join(name + " " + mapping[name] + "\n" for name in sorted(mapping)))
    stamp.write_text(json.dumps({"identity": identity, "archive_sha256": digest(archive),
                                 "symbols_sha256": digest(symbols)}, indent=2) + "\n")


if __name__ == "__main__":
    main()
