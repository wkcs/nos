#!/usr/bin/env python3
# Copyright (C) 2026 NOS contributors.
"""NOS ESP32 builds with repository-owned hardware sources and pinned tools."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import platform
import shlex
import subprocess
import sys
import tarfile
import tempfile
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
SDK_ROOT = Path(os.environ.get("NOS_ESP_SDK_ROOT", ROOT / "out/esp32-sdk")).resolve()
_source_spec = importlib.util.spec_from_file_location(
    "espressif_sources", ROOT / "scripts/espressif_sources.py"
)
sources = importlib.util.module_from_spec(_source_spec)
_source_spec.loader.exec_module(sources)
IDF = sources.location("esp-idf", SDK_ROOT)
TOOLS = SDK_ROOT / "tools"
IDF_COMMIT = "f5c3654a1c2d2a01f7f67def7a0dc48e691f63c0"
IDF_VERSION = "5.4.2"
ESP_USB_COMMIT = "68bf7f9613dbb1a31094fdb903209ae3e4bf388d"
ESP_USB = sources.location("esp-usb", SDK_ROOT)
XTENSA_VERSION = "esp-14.2.0_20241119"
CMAKE_VERSION = "3.30.2"
ULP_VERSION = "2.38_20240113"
QEMU_VERSION = "esp-develop-9.2.2-20260417"
QEMU_ARCHIVE = "qemu-xtensa-softmmu-esp_develop_9.2.2_20260417-x86_64-linux-gnu.tar.xz"
QEMU_SHA256 = "0eecb2a34a5586c0e59110f77b9343b7b336e82fdb0e1a30e1dc1bab8a547e35"
QEMU_URL = f"https://github.com/espressif/qemu/releases/download/{QEMU_VERSION}/{QEMU_ARCHIVE}"
TARGET = "esp32s3"
COMPILER_FAMILY = "xtensa-esp"
COMPILER_PREFIX = "xtensa-esp32s3"
QEMU_DIRECTORY = "qemu-s3"
QEMU_EXECUTABLE = "qemu-system-xtensa"


def configure_target(target):
    """Select one audited SDK port before running setup/build/tests."""
    global TARGET, COMPILER_FAMILY, COMPILER_PREFIX
    global QEMU_DIRECTORY, QEMU_EXECUTABLE, QEMU_ARCHIVE, QEMU_SHA256, QEMU_URL
    if target != "esp32c3":
        raise ValueError("Only the ESP32-C3 wrapper may override the S3 defaults")
    TARGET = target
    COMPILER_FAMILY = COMPILER_PREFIX = "riscv32-esp"
    QEMU_DIRECTORY = "qemu-c3"
    QEMU_EXECUTABLE = "qemu-system-riscv32"
    QEMU_ARCHIVE = "qemu-riscv32-softmmu-esp_develop_9.2.2_20260417-x86_64-linux-gnu.tar.xz"
    QEMU_SHA256 = "547f03e04701a92cbb699f7f7d015adc1f5b5ef93cbb94c0dd9b7107e2d84e77"
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
        TOOLS / f"tools/{COMPILER_FAMILY}-elf" / XTENSA_VERSION / f"{COMPILER_FAMILY}-elf/bin",
        TOOLS / "tools/cmake" / CMAKE_VERSION / "bin",
        TOOLS / "tools/esp32ulp-elf" / ULP_VERSION / "esp32ulp-elf/bin",
    ]
    cloud = Path("/workspace/.nos-env/root/usr")
    if cloud.exists():
        paths.append(cloud / "bin")
        env["LD_LIBRARY_PATH"] = str(cloud / "lib/x86_64-linux-gnu") + (
            ":" + env["LD_LIBRARY_PATH"] if env.get("LD_LIBRARY_PATH") else ""
        )
    env["PIP_CACHE_DIR"] = str(SDK_ROOT / "pip-cache")
    env["IDF_COMPONENT_CACHE_PATH"] = str(SDK_ROOT / "component-cache")
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
        IDF_SKIP_CHECK_SUBMODULES="1",
        NOS_ESP_USB_PATH=str(ESP_USB),
        ESP_ROM_ELF_DIR=str(TOOLS / "tools/esp-rom-elfs/20241011") + "/",
    )
    # We use the checked-in Python lock instead of the mutable CDN constraint file.
    return env


def verify_sdk():
    if sources.package("esp-idf")["commit"] != IDF_COMMIT:
        raise RuntimeError("The vendored IDF revision is outside the audited ABI")
    sources.prepare("esp-idf", SDK_ROOT)


def verify_usb():
    if sources.package("esp-usb")["commit"] != ESP_USB_COMMIT:
        raise RuntimeError("The vendored USB revision is outside the audited ABI")
    sources.prepare("esp-usb", SDK_ROOT)


def setup():
    if platform.system() != "Linux" or platform.machine() not in ("x86_64", "amd64"):
        raise RuntimeError("This reproducible tool bundle targets Linux x86_64")
    SDK_ROOT.mkdir(parents=True, exist_ok=True)
    verify_sdk()
    verify_usb()
    env = environment()
    run(
        [
            sys.executable,
            IDF / "tools/idf_tools.py",
            "install",
            f"{COMPILER_FAMILY}-elf",
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
            ROOT / "ports/esp32/python-requirements.lock",
        ],
        env=env,
    )
    qemu_dir = SDK_ROOT / QEMU_DIRECTORY
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
    print(f"{TARGET} SDK ready; run: make PLATFORM={TARGET} firmware")


def qemu_binary():
    return Path(
        os.environ.get("NOS_ESP_QEMU", SDK_ROOT / QEMU_DIRECTORY / "qemu/bin" / QEMU_EXECUTABLE)
    )


def sdkconfig_defaults(profile="runtime", storage="raw"):
    defaults = [ROOT / "ports/esp32/project/sdkconfig.defaults"]
    target_defaults = ROOT / "ports/esp32/project" / f"sdkconfig.{TARGET}.defaults"
    if target_defaults.exists():
        defaults.append(target_defaults)
    if profile == "ecosystem":
        defaults.append(ROOT / "ports/esp32/project/sdkconfig.ecosystem.defaults")
        if TARGET == "esp32s3":
            defaults.append(ROOT / "ports/esp32/project/sdkconfig.usb.defaults")
    if storage == "wl":
        defaults.append(ROOT / "ports/esp32/project/sdkconfig.storage-wl.defaults")
    return defaults


def build(
    profile="runtime", output=None, action="build", port=None, regression=False, storage="raw"
):
    verify_sdk()
    if profile == "ecosystem" and TARGET == "esp32s3":
        verify_usb()
    env = environment()
    python = python_environment() / "bin/python"
    if not python.exists():
        raise RuntimeError("SDK Python environment missing; run scripts/esp32s3.py setup")
    assert storage in ("raw", "wl")
    output = Path(
        output or ROOT / "out" / TARGET / (profile + ("-wl" if storage == "wl" else ""))
    ).resolve()
    output.mkdir(parents=True, exist_ok=True)
    mode = output / "storage-mode.txt"
    if mode.exists() and mode.read_text().strip() != storage:
        raise RuntimeError("Storage format changed; select a separate --output directory")
    if not mode.exists() and (output / "sdkconfig").exists() and storage != "raw":
        raise RuntimeError(
            "Existing output has raw configuration; select a separate --output directory"
        )
    mode.write_text(storage + "\n")
    defaults = sdkconfig_defaults(profile, storage)
    command = [
        python,
        IDF / "tools/idf.py",
        "-C",
        ROOT / "ports/esp32/project",
        "-B",
        output / "build",
        f"-DIDF_TARGET={TARGET}",
        f"-DSDKCONFIG={output / 'sdkconfig'}",
        "-DSDKCONFIG_DEFAULTS=" + ";".join(map(str, defaults)),
        "-DNOS_IDF_ECOSYSTEM=" + ("ON" if profile == "ecosystem" else "OFF"),
    ]
    command += ["-DNOS_IDF_REGRESSION=" + ("ON" if regression else "OFF")]
    command += ["-DNOS_IDF_STORAGE_WL=" + ("ON" if storage == "wl" else "OFF")]
    if port:
        command += ["-p", port]
    before = source_identity()
    run(command + [action], cwd=ROOT, env=env)
    if before != source_identity():
        raise RuntimeError("Repository inputs changed during ESP-IDF build")
    verify_sdk()
    if profile == "ecosystem" and TARGET == "esp32s3":
        verify_usb()
    target = output / "build"
    compiler = (
        TOOLS
        / f"tools/{COMPILER_FAMILY}-elf"
        / XTENSA_VERSION
        / f"{COMPILER_FAMILY}-elf/bin/{COMPILER_PREFIX}-elf-gcc"
    )
    manifest = {
        "source": before,
        "target": TARGET,
        "idf_commit": IDF_COMMIT,
        "hardware_bundle": sources.package("esp-idf")["sha256"],
        "boot_owner": "NOS",
        "startup": "NOS entry -> vendor CPU HAL -> NOS startup -> NOS kernel -> NOS init",
        "esp_usb_commit": (
            ESP_USB_COMMIT if profile == "ecosystem" and TARGET == "esp32s3" else None
        ),
        "profile": profile,
        "storage": storage,
        "regression": regression,
        "compiler_version": subprocess.check_output(
            [compiler, "--version"], text=True, env=env
        ).splitlines()[0],
        "python_lock_sha256": hashlib.sha256(
            (ROOT / "ports/esp32/python-requirements.lock").read_bytes()
        ).hexdigest(),
        "elf_sha256": hashlib.sha256((target / f"nos_{TARGET}.elf").read_bytes()).hexdigest(),
    }
    (target / "nos-build.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return target


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("setup", "env", "build", "test", "flash"))
    parser.add_argument("--profile", choices=("runtime", "ecosystem"), default="runtime")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--storage", choices=("raw", "wl"), default="raw")
    parser.add_argument("--port")
    parser.add_argument(
        "--regression",
        action="store_true",
        help="Test app: GPIO4, UART1, NVS, inactive OTA, Flash snapshots; WL adds 128 commits",
    )
    args = parser.parse_args()
    if args.action == "setup":
        setup()
    elif args.action == "env":
        verify_sdk()
        env = environment()
        for name in (
            "PATH",
            "LD_LIBRARY_PATH",
            "IDF_PATH",
            "IDF_TOOLS_PATH",
            "IDF_PYTHON_ENV_PATH",
            "ESP_IDF_VERSION",
            "ESP_ROM_ELF_DIR",
            "IDF_PYTHON_CHECK_CONSTRAINTS",
            "IDF_COMPONENT_MANAGER",
            "IDF_COMPONENT_CACHE_PATH",
            "NOS_ESP_USB_PATH",
        ):
            if name in env:
                print(f"export {name}={shlex.quote(env[name])}")
    elif args.action == "test":
        run([sys.executable, ROOT / "tests" / f"{TARGET}.py", "--storage", args.storage], cwd=ROOT)
    else:
        if args.action == "flash" and not args.port:
            parser.error("flash requires an explicit --port")
        build(args.profile, args.output, args.action, args.port, args.regression, args.storage)


if __name__ == "__main__":
    main()
