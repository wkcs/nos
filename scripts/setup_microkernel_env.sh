#!/usr/bin/env bash
# Copyright (C) 2026 NOS contributors. Unprivileged, verified Debian tool setup.
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
env_dir=/workspace/.nos-env
root_dir="$env_dir/root"
mkdir -p "$env_dir/apt/lists/partial" "$env_dir/apt/archives/partial" "$env_dir/logs" "$env_dir/musl-download"
cat > "$env_dir/apt/microkernel-sources.list" <<'EOF'
deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://deb.debian.org/debian trixie main
deb [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://deb.debian.org/debian-security trixie-security main
deb-src [signed-by=/usr/share/keyrings/debian-archive-keyring.gpg] https://deb.debian.org/debian trixie main
EOF
apt_opts=(
    -o Dir::Etc::sourcelist="$env_dir/apt/microkernel-sources.list"
    -o Dir::Etc::sourceparts=-
    -o Dir::State::lists="$env_dir/apt/lists"
    -o Dir::Cache::archives="$env_dir/apt/archives"
    -o Dir::Log="$env_dir/logs"
    -o Debug::NoLocking=1
)
/usr/bin/apt-get "${apt_opts[@]}" update
/usr/bin/apt-get "${apt_opts[@]}" --download-only --no-install-recommends --reinstall -y install \
    gcc-arm-none-eabi binutils-arm-none-eabi libnewlib-dev libnewlib-arm-none-eabi \
    gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu gcc-riscv64-linux-gnu \
    binutils-riscv64-linux-gnu gcc-riscv64-unknown-elf binutils-riscv64-unknown-elf \
    qemu-system-arm qemu-system-misc gdb-multiarch
mkdir -p "$root_dir"
for package in "$env_dir"/apt/archives/*.deb; do
    dpkg-deb --extract "$package" "$root_dir"
done
ln -sfn ../../include/newlib "$root_dir/usr/lib/arm-none-eabi/include"
ln -sfn newlib "$root_dir/usr/lib/arm-none-eabi/lib"
cat > "$env_dir/activate.sh" <<'EOF'
export PATH="/workspace/.nos-env/root/usr/bin:$PATH"
export LD_LIBRARY_PATH="/workspace/.nos-env/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QEMU_BINDIR=/workspace/.nos-env/root/usr/bin
EOF
source "$env_dir/activate.sh"
(
    cd "$env_dir/musl-download"
    /usr/bin/apt-get "${apt_opts[@]}" --download-only source musl
)
cd "$repo_dir"
for platform in arm64_virt riscv_virt; do
    python3 scripts/build_musl.py --archive "$env_dir/musl-download/musl_1.2.5.orig.tar.gz" \
        --platform "$platform" > "$env_dir/logs/musl-$platform-build.log" 2>&1 || {
        tail -40 "$env_dir/logs/musl-$platform-build.log"
        exit 1
    }
done
for platform in arm_virt stm32; do
    python3 scripts/build_newlib.py --platform "$platform" > "$env_dir/logs/newlib-$platform-build.log" 2>&1 || {
        tail -40 "$env_dir/logs/newlib-$platform-build.log"
        exit 1
    }
done
printf 'Activate tools: source %s/activate.sh\n' "$env_dir"
