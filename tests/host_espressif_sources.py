#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors.
"""Exercise offline source installation and reject corrupted build inputs."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("esp_port", ROOT / "scripts/esp32s3.py")
port = importlib.util.module_from_spec(spec)
spec.loader.exec_module(port)
sources = port.sources


def rejected(action):
    try:
        action()
    except (RuntimeError, tarfile.FilterError):
        return
    raise AssertionError("Corrupt source input was accepted")


def main():
    before = port.source_identity()
    checks = []
    original = sources.BUNDLE
    with tempfile.TemporaryDirectory(prefix="nos-vendor-test-") as temporary:
        root = Path(temporary)
        sources.BUNDLE = root / "bundle"
        sources.BUNDLE.mkdir()
        archive = sources.BUNDLE / "fixture.tar.xz"
        payload = b"/* offline hardware source */\n"
        with tarfile.open(archive, "w:xz") as output:
            entry = tarfile.TarInfo("driver.c")
            entry.size = len(payload)
            entry.mode = 0o755
            output.addfile(entry, io.BytesIO(payload))
            entry = tarfile.TarInfo("alias.c")
            entry.type = tarfile.SYMTYPE
            entry.linkname = "driver.c"
            output.addfile(entry)
        inventory = sources.BUNDLE / "inventory.json"
        inventory.write_text(
            json.dumps(
                {
                    "files": {
                        "driver.c": {
                            "sha256": hashlib.sha256(payload).hexdigest(),
                            "executable": True,
                        }
                    },
                    "symlinks": {"alias.c": "driver.c"},
                }
            )
        )
        item = {
            "name": "fixture",
            "archive": archive.name,
            "sha256": sources.sha256(archive),
            "inventory": inventory.name,
            "inventory_sha256": sources.sha256(inventory),
            "generated_files": {"version.txt": "v5.4.2\n"},
        }
        manifest = sources.BUNDLE / "manifest.json"

        def save():
            manifest.write_text(json.dumps({"format": 1, "packages": [item]}))

        save()
        installed = sources.prepare("fixture", root / "cache")
        assert (installed / "driver.c").read_bytes() == payload
        assert (installed / "alias.c").is_symlink()
        assert (installed / "version.txt").read_text() == "v5.4.2\n"
        assert sources.prepare("fixture", root / "cache") == installed
        checks.append("offline install, executable, symlink, release metadata and repeat install")

        (installed / "driver.c").write_bytes(b"changed")
        rejected(lambda: sources.prepare("fixture", root / "cache"))
        (installed / "driver.c").write_bytes(payload)
        checks.append("source corruption rejected")

        (installed / "driver.c").chmod(0o644)
        rejected(lambda: sources.prepare("fixture", root / "cache"))
        (installed / "driver.c").chmod(0o755)
        checks.append("executable mode corruption rejected")

        (installed / "extra.c").write_text("unexpected")
        rejected(lambda: sources.prepare("fixture", root / "cache"))
        (installed / "extra.c").unlink()
        checks.append("unexpected source rejected")

        (installed / "alias.c").unlink()
        (installed / "alias.c").symlink_to("version.txt")
        rejected(lambda: sources.prepare("fixture", root / "cache"))
        (installed / "alias.c").unlink()
        (installed / "alias.c").symlink_to("driver.c")
        checks.append("symlink substitution rejected")

        (installed / "version.txt").write_text("wrong version")
        rejected(lambda: sources.prepare("fixture", root / "cache"))
        (installed / "version.txt").write_text("v5.4.2\n")
        checks.append("release metadata corruption rejected")

        inventory.write_text("corrupt inventory")
        rejected(lambda: sources.prepare("fixture", root / "cache"))
        checks.append("inventory digest mismatch rejected")

        archive.write_bytes(b"corrupt archive")
        rejected(lambda: sources.prepare("fixture", root / "cache"))
        checks.append("archive digest mismatch rejected")
    sources.BUNDLE = original
    # Exercise the actual bundled firmware dependencies, not only fixtures.
    port.verify_sdk()
    port.verify_usb()
    assert (port.IDF / "version.txt").read_text() == "v5.4.2\n"
    checks.append("actual IDF and USB bundle inventories verified")
    assert before == port.source_identity()
    result = {"passed": True, "source": before, "checks": checks}
    report = ROOT / "out/esp32-owned-startup" / str(time.time_ns()) / "sources.json"
    report.parent.mkdir(parents=True)
    report.write_text(json.dumps(result, indent=2) + "\n")
    print(report)
    print(f"PASS {len(checks)} offline source integrity groups")


if __name__ == "__main__":
    main()
