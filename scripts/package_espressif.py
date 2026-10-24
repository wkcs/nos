#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors.
"""Reproduce the checked-in hardware bundle from clean pinned checkouts."""
import argparse
import hashlib, json, subprocess, tarfile
from pathlib import Path

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--idf-source", type=Path, required=True)
parser.add_argument("--usb-source", type=Path, required=True)
args = parser.parse_args()
out = root / "third_party/espressif"
packages = []
for name, source, commit, url in [
    (
        "esp-idf",
        args.idf_source.resolve(),
        "f5c3654a1c2d2a01f7f67def7a0dc48e691f63c0",
        "https://github.com/espressif/esp-idf",
    ),
    (
        "esp-usb",
        args.usb_source.resolve(),
        "68bf7f9613dbb1a31094fdb903209ae3e4bf388d",
        "https://github.com/espressif/esp-usb",
    ),
]:
    actual = subprocess.check_output(
        ["git", "-C", str(source), "rev-parse", "HEAD"], text=True
    ).strip()
    if actual != commit:
        raise RuntimeError(f"Wrong upstream commit: {source}")
    if subprocess.check_output(
        ["git", "-C", str(source), "status", "--porcelain", "--untracked-files=no"], text=True
    ):
        raise RuntimeError(f"Dirty upstream: {source}")
    files = {}
    links = {}
    archive = out / (name + "-sources.tar.xz")
    excluded = {".git", "__pycache__"}
    if name == "esp-idf":
        excluded |= {"examples", "docs"}
    with tarfile.open(archive, "w:xz", preset=6) as tar:
        for path in sorted(source.rglob("*")):
            rel = path.relative_to(source)
            if any(p in excluded for p in rel.parts) or path.name.endswith(".pyc"):
                continue
            if not path.is_file() and not path.is_symlink():
                continue
            entry = tar.gettarinfo(str(path), str(rel))
            entry.uid = entry.gid = entry.mtime = 0
            entry.uname = entry.gname = ""
            if path.is_symlink():
                links[str(rel)] = entry.linkname
                tar.addfile(entry)
            else:
                files[str(rel)] = {
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                    "executable": bool(entry.mode & 0o111),
                }
                with path.open("rb") as data:
                    tar.addfile(entry, data)
    inventory = out / (name + "-inventory.json")
    inventory.write_text(
        json.dumps({"files": files, "symlinks": links}, sort_keys=True, indent=2) + "\n"
    )
    submodules = subprocess.check_output(
        ["git", "-C", str(source), "submodule", "status", "--recursive"], text=True
    )
    packages.append(
        {
            "name": name,
            **({"generated_files": {"version.txt": "v5.4.2\n"}} if name == "esp-idf" else {}),
            "upstream": url,
            "commit": commit,
            "archive": archive.name,
            "sha256": hashlib.sha256(archive.read_bytes()).hexdigest(),
            "bytes": archive.stat().st_size,
            "inventory": inventory.name,
            "inventory_sha256": hashlib.sha256(inventory.read_bytes()).hexdigest(),
            "submodules": submodules.splitlines(),
            "excluded": [".git", "__pycache__", "*.pyc"]
            + (["examples", "docs"] if name == "esp-idf" else []),
        }
    )
    print(name, archive.stat().st_size, len(files), flush=True)
(out / "manifest.json").write_text(json.dumps({"format": 1, "packages": packages}, indent=2) + "\n")
