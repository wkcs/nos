#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors.
"""Verify/unpack the repository-owned Espressif source bundle, without network."""
import hashlib
import json
import os
from pathlib import Path
import tarfile
import tempfile

ROOT = Path(__file__).resolve().parents[1]
BUNDLE = ROOT / "third_party/espressif"


def sha256(path):
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def package(name):
    manifest = json.loads((BUNDLE / "manifest.json").read_text())
    return next(p for p in manifest["packages"] if p["name"] == name)


def location(name, cache):
    item = package(name)
    identity = hashlib.sha256(json.dumps(item, sort_keys=True).encode()).hexdigest()
    return Path(cache) / "sources" / identity / name


def verify_tree(path, inventory):
    for name, record in inventory["files"].items():
        source = path / name
        if source.is_symlink() or not source.is_file() or sha256(source) != record["sha256"]:
            raise RuntimeError(f"Vendored source changed or missing: {source}")
        if bool(source.stat().st_mode & 0o111) != record["executable"]:
            raise RuntimeError(f"Vendored executable mode changed: {source}")
    for name, target in inventory["symlinks"].items():
        source = path / name
        if not source.is_symlink() or os.readlink(source) != target:
            raise RuntimeError(f"Vendored symlink changed: {source}")
    expected = set(inventory["files"]) | set(inventory["symlinks"])
    for source in path.rglob("*"):
        relative = source.relative_to(path)
        if "__pycache__" in relative.parts or source.suffix == ".pyc":
            continue
        if (source.is_file() or source.is_symlink()) and str(relative) not in expected:
            raise RuntimeError(f"Unexpected vendored source: {source}")


def prepare(name, cache):
    item = package(name)
    archive = BUNDLE / item["archive"]
    inventory_path = BUNDLE / item["inventory"]
    for path, expected in ((archive, item["sha256"]), (inventory_path, item["inventory_sha256"])):
        if sha256(path) != expected:
            raise RuntimeError(f"Espressif bundle checksum mismatch: {path}")
    inventory = json.loads(inventory_path.read_text())
    # IDF's supported release-archive version file prevents git-describe from
    # accidentally reporting the enclosing NOS repository as the SDK version.
    for filename, value in item.get("generated_files", {}).items():
        if Path(filename).name != filename or filename in inventory["files"]:
            raise RuntimeError(f"Invalid generated metadata filename: {filename}")
        inventory["files"][filename] = {
            "sha256": hashlib.sha256(value.encode()).hexdigest(),
            "executable": False,
        }
    destination = location(name, cache)
    destination.parent.mkdir(parents=True, exist_ok=True)
    if not destination.exists():
        with tempfile.TemporaryDirectory(prefix="unpack-", dir=destination.parent) as temporary:
            stage = Path(temporary) / name
            stage.mkdir()
            with tarfile.open(archive) as source:
                source.extractall(stage, filter="data")
            for filename, value in item.get("generated_files", {}).items():
                (stage / filename).write_text(value)
            verify_tree(stage, inventory)
            try:
                stage.rename(destination)
            except OSError:
                if not destination.is_dir():
                    raise
                # Another target installed the same snapshot; verify it below.
    verify_tree(destination, inventory)
    return destination
