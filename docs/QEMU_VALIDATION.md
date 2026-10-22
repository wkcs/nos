# 构建与 QEMU 验证

<!-- Copyright (C) 2026 NOS contributors -->

首次准备或刷新环境：

```sh
bash scripts/setup_microkernel_env.sh
source /workspace/.nos-env/activate.sh
```

脚本将 Debian 签名索引验证的交叉编译器和原版 QEMU 提取到 `/workspace/.nos-env/root`，并构建固定 SHA256 的真实 Newlib 4.5.0、musl 1.2.5。无需改动系统安装目录。保留包签名、源码校验与 TLS 验证；不使用历史 `nos_stm32` 补丁。

| 平台 | 原版 QEMU / CPU | 内存模型 | 库 | CPU 数 |
|---|---|---|---|---|
| `arm_virt` | `virt,gic-version=2` / Cortex-A15 | ARMv7 MMU | minimal、Newlib | 1–4 |
| `arm64_virt` | `virt,gic-version=2` / Cortex-A53 | ARM64 MMU | minimal、musl | 1–4 |
| `riscv_virt` | `virt` / RV64GC，`-bios none` | Sv39 | minimal、musl | 1–4 |
| `stm32` | `netduinoplus2` / STM32F405 Cortex-M4 | 非 MMU，可选 MPU | minimal、Newlib | 1 |
| `riscv32_virt` | `virt` / RV32GC，`-bios none` | Sv32 | minimal | 1–4 |
| `riscv32_nommu` | `virt` / RV32GC，`-bios none` | 关闭地址翻译，共享地址空间 | minimal | 1–4 |

RV32 使用 `riscv64-unknown-elf` multilib 编译器；PIE 链接还需要支持 ELF32 的 Linux RISC-V binutils。自备工具链可通过 Make 的 `ARM_PREFIX`、`ARM64_PREFIX`、`RISCV_PREFIX`、`RISCV32_PREFIX` 和 `QEMU_BINDIR` 覆盖；具体参数以 `scripts/Makefile.microkernel` 为准。源码构建使用 `-Wall -Wextra -Werror`。

## 常用命令

```sh
make PLATFORM=arm64_virt LIBC=musl -j5
make PLATFORM=arm_virt LIBC=newlib SMP=4 -j5
make PLATFORM=stm32 LIBC=newlib MPU=1 qemu-run
make PLATFORM=arm64_virt LIBC=minimal BUILD_MODE=normal qemu-run
make qemu-test
python3 tests/acceptance.py
```

`make qemu-test` 是原有四个平台 × 两种运行库，共八个基础组合。`tests/acceptance.py` 默认运行阶段 0–5 的基础软件验收；可用重复的 `--section` 做开发期间的局部检查。普通系统使用 `BUILD_MODE=normal`，运行独立 ELF 后提供持续存活的 init/UART shell；自测模式才使用测试专用结束接口。

```sh
python3 tests/qemu_matrix.py --platform stm32 --libc all --mpu
python3 tests/qemu_matrix.py --platform riscv_virt --libc musl --cpus 4
python3 tests/qemu_matrix.py --platform riscv32_nommu --libc minimal
python3 tests/qemu_persistence.py --platform arm64_virt --filesystem fat --virtio-version modern
python3 tests/acceptance.py --section normal --section storage
```

Newlib 默认使用源码构建的动态重入端口，支持 retargetable locks。`NEWLIB_MODE=stock` 仅用于诊断，不能作为跨核重入验证。独立构建库可查看 `scripts/build_newlib.py --help`、`scripts/build_musl.py --help`；Lua/LVGL 使用 `scripts/build_user_libraries.py --platform arm64_virt|riscv_virt`。

## 完整软件验收范围

| 工作线 | 检查 |
|---|---|
| 基础矩阵 | 八组合；1000 轮资源/端点回收、IPC/锁/权限、线程、C 库、ELF、内存与错误路径 |
| 构建 | 配置变化、无变化增量、真实静态库更新；VFS 宿主 ASAN/UBSAN 与有界随机输入 |
| 正常启动 | 三个主架构 UART 输入、独立程序退出后 init 存活、引号、行编辑、历史、错误输入、两次 VFS/UART 重启及资源基线；STM32 默认只验证启动 |
| 持久化 | 三架构 × native/FAT × legacy/modern 共 12 组合，加 native 损坏回退；每项两次真实启动、真实 virtio IRQ |
| SMP | 三主架构 × minimal/真实 libc × 2/4 CPU，共 12 组合；实际用户态并行、锁、TLS、stdio、TLB 同步及宿主时钟核对 |
| MPU / RV32 | STM32 minimal/Newlib MPU 权限故障；RV32 Sv32 和非 MMU minimal |
| 64 位时间 | ARM、STM32、RV32 两模式从 `0x180000000` tick 启动，验证高位快照、旧接口边界及睡眠进展 |
| 特权异常 | 六平台故障注入，必须匹配指定 panic、FAIL 和预期 QEMU 退出码；超时不是通过 |
| 应用 | ARM64/RV64 musl 实际执行独立 Lua/LVGL、TLS；LVGL 为内存渲染验证 |

基础验收为 54 个命令组；加上 `--include-posix` 共 72 组，包含 nshell 宿主消毒器与三架构交互验证，以及 RV64/RV32 两种内存模式的 UART pending 重使能回归。最终完整验收命令为 `python3 tests/acceptance.py --include-posix --require-frozen-source`。宿主 VFS 检查不替代实际 IPC/硬件来宾验证。STM32 非 MPU 和 RV32 非 MMU 不执行 MMU 进程隔离检查；可选 MPU 也不等于 MMU 的任意进程地址空间。

## 证据与判定

每次 `qemu_matrix.py` 保留不可覆盖的 `out/microkernel/reports/<run_id>/`，含 build/QEMU 日志、配置、镜像 SHA256、代码状态、工具与实际 libc 身份。必须同时满足完整必需标记、无 FAIL、最终 `NOS TEST PASS`、QEMU 退出 0；SMP 还核对宿主经过时间。故障注入的预期失败由专门 runner 判断。

完整八组合才更新 `out/microkernel/results.json`；局部测试更新 `results-partial.json`，二者均保留独立历史。最终验收汇总位于 `out/microkernel/acceptance/<run_id>/results.json`；核对 `completed == total` 以及每项结果，不能只看中途的 `passed: true`。`source_changed_during_run` 记录运行期间输入是否变化，最终验收期间应冻结源码与构建输入。

`normal-validation/` 保存正常运行报告；`persistence/<run_id>/` 保存两次启动、磁盘与镜像身份；验收目录日志关联这些报告。真实开发板串口 runner 是 `tests/stm32_hardware.py`，没有板子时明确记录未执行。远端 CI、物理板、USB/显示/输入硬件、完整 POSIX 均须另行验证。当前结果见 [VALIDATION_RESULTS.md](VALIDATION_RESULTS.md)，支持范围见 [POSIX_SUPPORT.md](POSIX_SUPPORT.md)。
