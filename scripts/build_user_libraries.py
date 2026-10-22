#!/usr/bin/env python3
"""Copyright (C) 2026 NOS contributors. Build existing libraries for user ELF apps."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

ROOT = Path(__file__).resolve().parents[1]
TARGETS = {
    "arm64_virt": ("aarch64-linux-gnu-", ["-mcpu=cortex-a53", "-mno-outline-atomics"]),
    "riscv_virt": ("riscv64-linux-gnu-", ["-march=rv64gc", "-mabi=lp64d", "-mcmodel=medany", "-mno-relax", "-msmall-data-limit=0"]),
}


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command):
    result = subprocess.run(command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise RuntimeError("Command failed: " + " ".join(map(str, command)) + "\n" + result.stdout[-8000:])


def build(name, platform, destination, musl_root, jobs):
    prefix, cpu = TARGETS[platform]
    compiler = shutil.which(prefix + "gcc")
    archiver = shutil.which(prefix + "ar")
    if not compiler or not archiver:
        raise RuntimeError("Activate the NOS toolchain environment first")
    if not (musl_root / "include" / "stdio.h").is_file():
        raise RuntimeError("Build the corresponding musl port first")
    if name == "lua":
        library_root = ROOT / "third_party/lua"
        omitted = {"lua.c", "luac.c", "linit.c", "liolib.c", "loslib.c", "loadlib.c", "ldblib.c", "lcorolib.c"}
        sources = sorted(p for p in library_root.glob("*.c") if p.name not in omitted)
        options = ["-I" + str(library_root)]
        configuration = []
    else:
        library_root = ROOT / "third_party/lvgl"
        sources = sorted((library_root / "src").rglob("*.c"))
        options = ["-DLV_CONF_INCLUDE_SIMPLE", "-I" + str(ROOT / "apps/lvgl"), "-I" + str(library_root)]
        configuration = [ROOT / "apps/lvgl/lv_conf.h"]
    flags = cpu + ["-O2", "-std=gnu11", "-ffreestanding", "-fno-pie", "-fno-pic", "-fno-stack-protector",
                   "-ffunction-sections", "-fdata-sections", "-isystem", str(musl_root / "include")] + options
    inputs = sources + sorted(library_root.rglob("*.h")) + sorted((musl_root / "include").rglob("*.h")) + configuration + [Path(__file__)]
    identity = {
        "compiler": compiler,
        "compiler_sha256": digest(Path(compiler)),
        "compiler_version": subprocess.check_output([compiler, "--version"], text=True).splitlines()[0],
        "flags": flags,
        "inputs": {str(p.relative_to(ROOT)): digest(p) for p in inputs},
    }
    directory = destination / name
    directory.mkdir(parents=True, exist_ok=True)
    archive = directory / ("lib" + name + ".a")
    manifest = directory / "manifest.json"
    if manifest.is_file() and archive.is_file():
        previous = json.loads(manifest.read_text())
        if previous.get("identity") == identity and previous.get("archive_sha256") == digest(archive):
            print(str(archive.relative_to(ROOT)) + " (unchanged)")
            return

    def compile_source(source):
        target = directory / "objects" / source.relative_to(library_root).with_suffix(".o")
        target.parent.mkdir(parents=True, exist_ok=True)
        run([compiler] + flags + ["-c", str(source), "-o", str(target)])
        return target

    with ThreadPoolExecutor(max_workers=jobs) as workers:
        objects = list(workers.map(compile_source, sources))
    # Removing the old archive also removes members for sources no longer selected.
    archive.unlink(missing_ok=True)
    run([archiver, "rcsD", str(archive)] + [str(p) for p in objects])
    manifest.write_text(json.dumps({"identity": identity, "archive_sha256": digest(archive)}, indent=2) + "\n")
    print(str(archive.relative_to(ROOT)) + " (" + str(len(objects)) + " objects)")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", required=True, choices=TARGETS)
    parser.add_argument("--library", choices=("all", "lua", "lvgl"), default="all")
    parser.add_argument("--musl-root", type=Path)
    parser.add_argument("--output", type=Path)
    parser.add_argument("--jobs", type=int, default=5)
    args = parser.parse_args()
    if args.jobs < 1 or args.jobs > 32:
        parser.error("--jobs must be between 1 and 32")
    musl_root = (args.musl_root or ROOT / "out/microkernel/musl" / args.platform / "install").resolve()
    destination = (args.output or ROOT / "out/microkernel/userlibs" / args.platform).resolve()
    # Relative paths in manifests stay portable within the shared repository.
    if not musl_root.is_relative_to(ROOT) or not destination.is_relative_to(ROOT):
        parser.error("--musl-root and --output must be within the repository")
    for name in ("lua", "lvgl") if args.library == "all" else (args.library,):
        build(name, args.platform, destination, musl_root, args.jobs)


if __name__ == "__main__":
    main()
