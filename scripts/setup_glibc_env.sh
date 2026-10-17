#!/usr/bin/env bash
# Copyright (C) 2026 NOS contributors. Verified Debian Linux ABI toolchains.
set -euo pipefail
repo_dir=$(cd "$(dirname "$0")/.." && pwd)
env_dir=/workspace/.nos-env
mkdir -p "$env_dir/apt/lists/partial" "$env_dir/apt/archives/partial" "$env_dir/logs" "$env_dir/glibc-source"
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
    gcc-arm-linux-gnueabi binutils-arm-linux-gnueabi \
    gcc-aarch64-linux-gnu binutils-aarch64-linux-gnu gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu \
    libc6-armel-cross libc6-dev-armel-cross libc6-arm64-cross libc6-dev-arm64-cross \
    libc6-riscv64-cross libc6-dev-riscv64-cross \
    linux-libc-dev-armel-cross linux-libc-dev-arm64-cross linux-libc-dev-riscv64-cross
(
    cd "$env_dir/glibc-source"
    /usr/bin/apt-get "${apt_opts[@]}" --download-only source glibc=2.41-11
)
cd "$repo_dir"
# The Python stage verifies package/source SHA256 against the signed APT indexes
# before extraction and records the exact unmodified shared object bytes.
python3 scripts/glibc_artifacts.py --install
printf 'glibc manifest: %s/glibc/manifest.json\n' "$env_dir"
