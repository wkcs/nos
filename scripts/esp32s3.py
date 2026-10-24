#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors.
"""Pinned ESP32-S3 SDK setup and NOS builds; downloads stay under ignored out/."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SDK_ROOT = Path(os.environ.get("NOS_ESP_SDK_ROOT", ROOT / "out/esp32-sdk")).resolve()
IDF = SDK_ROOT / "esp-idf"
TOOLS = SDK_ROOT / "tools"
IDF_COMMIT = "f5c3654a1c2d2a01f7f67def7a0dc48e691f63c0"
IDF_VERSION = "5.4.2"
ESP_USB_COMMIT = "68bf7f9613dbb1a31094fdb903209ae3e4bf388d"
ESP_USB = SDK_ROOT / "esp-usb"
XTENSA_VERSION = "esp-14.2.0_20241119"
CMAKE_VERSION = "3.30.2"
QEMU_VERSION = "esp-develop-9.2.2-20260417"
QEMU_ARCHIVE = "qemu-xtensa-softmmu-esp_develop_9.2.2_20260417-x86_64-linux-gnu.tar.xz"
QEMU_SHA256 = "0eecb2a34a5586c0e59110f77b9343b7b336e82fdb0e1a30e1dc1bab8a547e35"
QEMU_URL = f"https://github.com/espressif/qemu/releases/download/{QEMU_VERSION}/{QEMU_ARCHIVE}"


def source_identity():
    def git(*args):
        return subprocess.check_output(["git", *args], cwd=ROOT)

    names = git("ls-files", "--others", "--exclude-standard", "-z").decode().split("\0")
    return {
        "commit": git("rev-parse", "HEAD").decode().strip(),
        "status": git("status", "--porcelain").decode().splitlines(),
        "tracked_diff_sha256": hashlib.sha256(git("diff", "--binary", "HEAD")).hexdigest(),
        "untracked_sha256": {
            name: hashlib.sha256((ROOT / name).read_bytes()).hexdigest()
            for name in names
            if name and (ROOT / name).is_file()
        },
    }


def run(command, **kwargs):
    print("+", " ".join(map(str, command)), flush=True)
    subprocess.run(list(map(str, command)), check=True, **kwargs)


def python_environment():
    return TOOLS / "python_env" / f"idf5.4_py{sys.version_info.major}.{sys.version_info.minor}_env"


def environment():
    env = os.environ.copy()
    venv = python_environment()
    paths = [
        venv / "bin",
        TOOLS / "tools/xtensa-esp-elf" / XTENSA_VERSION / "xtensa-esp-elf/bin",
        TOOLS / "tools/cmake" / CMAKE_VERSION / "bin",
    ]
    cloud = Path("/workspace/.nos-env/root/usr")
    if cloud.exists():
        paths.append(cloud / "bin")
        env["LD_LIBRARY_PATH"] = str(cloud / "lib/x86_64-linux-gnu") + (
            ":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else ""
        )
    env["PIP_CACHE_DIR"] = str(SDK_ROOT / "pip-cache")
    env["PIP_DISABLE_PIP_VERSION_CHECK"] = "1"
    env["PIP_EXTRA_INDEX_URL"] = ""
    env["PATH"] = os.pathsep.join(map(str, paths)) + os.pathsep + env["PATH"]
    env.update(
        IDF_PATH=str(IDF),
        IDF_TOOLS_PATH=str(TOOLS),
        IDF_VERSION=IDF_VERSION,
        ESP_IDF_VERSION=IDF_VERSION,
        IDF_PYTHON_ENV_PATH=str(venv),
        IDF_PYTHON_CHECK_CONSTRAINTS="no",
        IDF_COMPONENT_MANAGER="0",
        NOS_ESP_USB_PATH=str(ESP_USB),
        ESP_ROM_ELF_DIR=str(TOOLS / "tools/esp-rom-elfs/20241011") + "/",
    )
    # We use the checked-in Python lock instead of the mutable CDN constraint file.
    return env


def verify_sdk():
    actual = subprocess.check_output(
        ["git", "-C", str(IDF), "rev-parse", "HEAD"], text=True
    ).strip()
    if actual != IDF_COMMIT:
        raise RuntimeError(f"Expected ESP-IDF {IDF_COMMIT}, got {actual}")
    changes = subprocess.check_output(
        ["git", "-C", str(IDF), "status", "--porcelain", "--untracked-files=no"], text=True
    )
    if changes:
        raise RuntimeError(
            "ESP-IDF checkout has local changes; preserve them in a separate SDK path"
        )


def verify_usb():
    actual = subprocess.check_output(
        ["git", "-C", str(ESP_USB), "rev-parse", "HEAD"], text=True
    ).strip()
    if actual != ESP_USB_COMMIT:
        raise RuntimeError(f"Expected esp-usb {ESP_USB_COMMIT}, got {actual}")
    changes = subprocess.check_output(
        ["git", "-C", str(ESP_USB), "status", "--porcelain", "--untracked-files=no"], text=True
    )
    if changes:
        raise RuntimeError("esp-usb checkout has local changes")


def setup():
    if platform.system() != "Linux" or platform.machine() not in ("x86_64", "amd64"):
        raise RuntimeError("This reproducible tool bundle targets Linux x86_64")
    SDK_ROOT.mkdir(parents=True, exist_ok=True)
    if not IDF.exists():
        run(
            [
                "git",
                "clone",
                "--depth",
                "1",
                "--branch",
                "v5.4.2",
                "--filter=blob:none",
                "https://github.com/espressif/esp-idf.git",
                IDF,
            ]
        )
    verify_sdk()
    run(["git", "-C", IDF, "submodule", "update", "--init", "--depth", "1", "--jobs", "4"])
    if not ESP_USB.exists():
        run(["git", "init", ESP_USB])
        run(
            [
                "git",
                "-C",
                ESP_USB,
                "remote",
                "add",
                "origin",
                "https://github.com/espressif/esp-usb.git",
            ]
        )
        run(["git", "-C", ESP_USB, "fetch", "--depth", "1", "origin", ESP_USB_COMMIT])
        run(["git", "-C", ESP_USB, "checkout", "--detach", "FETCH_HEAD"])
    verify_usb()
    run(
        [
            "git",
            "-C",
            ESP_USB,
            "submodule",
            "update",
            "--init",
            "--depth",
            "1",
            "device/esp_tinyusb/tinyusb",
        ]
    )
    env = environment()
    run(
        [
            sys.executable,
            IDF / "tools/idf_tools.py",
            "install",
            "xtensa-esp-elf",
            "esp32ulp-elf",
            "cmake",
            "esp-rom-elfs",
        ],
        env=env,
    )
    venv = python_environment()
    if not (venv / "bin/python").exists():
        run([sys.executable, "-m", "venv", venv])
    run(
        [
            venv / "bin/python",
            "-m",
            "pip",
            "install",
            "--index-url",
            "https://pypi.org/simple",
            "-r",
            ROOT / "ports/esp_idf/python-requirements.lock",
        ],
        env=env,
    )
    qemu_dir = SDK_ROOT / "qemu-s3"
    qemu_dir.mkdir(exist_ok=True)
    archive = qemu_dir / QEMU_ARCHIVE
    if not archive.exists():
        with urllib.request.urlopen(QEMU_URL) as response, archive.open("wb") as output:
            while data := response.read(1024 * 1024):
                output.write(data)
    if hashlib.sha256(archive.read_bytes()).hexdigest() != QEMU_SHA256:
        raise RuntimeError(f"QEMU archive checksum mismatch: {archive}")
    # Do not truncate a running emulator when setup is repeated. Stage files
    # from the verified archive and replace each regular file atomically.
    with tempfile.TemporaryDirectory(prefix="install-", dir=qemu_dir) as temporary:
        stage = Path(temporary)
        with tarfile.open(archive) as source:
            source.extractall(stage, filter="data")
        for path in sorted(stage.rglob("*")):
            target = qemu_dir / path.relative_to(stage)
            if path.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                os.replace(path, target)
    (qemu_dir / "download.json").write_text(
        json.dumps({"url": QEMU_URL, "sha256": QEMU_SHA256}, indent=2) + "\n"
    )
    run([qemu_binary(), "--version"], env=env)
    print("ESP32-S3 SDK ready; run: make PLATFORM=esp32s3 firmware")


def qemu_binary():
    return Path(os.environ.get("NOS_ESP_QEMU", SDK_ROOT / "qemu-s3/qemu/bin/qemu-system-xtensa"))


def build(profile="runtime", output=None, action="build", port=None, regression=False):
    verify_sdk()
    if profile == "ecosystem":
        verify_usb()
    env = environment()
    python = python_environment() / "bin/python"
    if not python.exists():
        raise RuntimeError("SDK Python environment missing; run scripts/esp32s3.py setup")
    output = Path(output or ROOT / "out/esp32s3" / profile).resolve()
    output.mkdir(parents=True, exist_ok=True)
    defaults = [ROOT / "ports/esp_idf/project/sdkconfig.defaults"]
    if profile == "ecosystem":
        defaults.append(ROOT / "ports/esp_idf/project/sdkconfig.ecosystem.defaults")
    command = [
        python,
        IDF / "tools/idf.py",
        "-C",
        ROOT / "ports/esp_idf/project",
        "-B",
        output / "build",
        "-DIDF_TARGET=esp32s3",
        f"-DSDKCONFIG={output / 'sdkconfig'}",
        "-DSDKCONFIG_DEFAULTS=" + ";".join(map(str, defaults)),
        "-DNOS_IDF_ECOSYSTEM=" + ("ON" if profile == "ecosystem" else "OFF"),
    ]
    command += ["-DNOS_IDF_REGRESSION=" + ("ON" if regression else "OFF")]
    if port:
        command += ["-p", port]
    before = source_identity()
    run(command + [action], cwd=ROOT, env=env)
    if before != source_identity():
        raise RuntimeError("Repository inputs changed during ESP32-S3 build")
    verify_sdk()
    if profile == "ecosystem":
        verify_usb()
    target = output / "build"
    compiler = (
        TOOLS
        / "tools/xtensa-esp-elf"
        / XTENSA_VERSION
        / "xtensa-esp-elf/bin/xtensa-esp32s3-elf-gcc"
    )
    manifest = {
        "source": before,
        "idf_commit": IDF_COMMIT,
        "esp_usb_commit": ESP_USB_COMMIT if profile == "ecosystem" else None,
        "profile": profile,
        "regression": regression,
        "compiler_version": subprocess.check_output(
            [compiler, "--version"], text=True, env=env
        ).splitlines()[0],
        "python_lock_sha256": hashlib.sha256(
            (ROOT / "ports/esp_idf/python-requirements.lock").read_bytes()
        ).hexdigest(),
        "elf_sha256": hashlib.sha256((target / "nos_esp32s3.elf").read_bytes()).hexdigest(),
    }
    (target / "nos-build.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("setup", "build", "test", "flash"))
    parser.add_argument("--profile", choices=("runtime", "ecosystem"), default="runtime")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--port")
    parser.add_argument(
        "--regression",
        action="store_true",
        help="Test app: GPIO4, UART1, NVS and inactive OTA writes",
    )
    args = parser.parse_args()
    if args.action == "setup":
        setup()
    elif args.action == "test":
        run([sys.executable, ROOT / "tests/esp32s3.py"], cwd=ROOT)
    else:
        if args.action == "flash" and not args.port:
            parser.error("flash requires an explicit --port")
        build(args.profile, args.output, args.action, args.port, args.regression)


if __name__ == "__main__":
    main()
