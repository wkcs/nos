# NOS

支持 ARM、ARM64、RISC-V 的微内核，包含能力控制 IPC、抢占调度、线程/锁、物理与虚拟内存、独立 ELF，以及用户态 VFS、UART 和 virtio-blk 服务。

```sh
bash scripts/setup_microkernel_env.sh
source /workspace/.nos-env/activate.sh
make qemu-test
python3 tests/acceptance.py --include-posix --require-frozen-source
```

原版 QEMU 验证覆盖 ARMv7、ARM64、RV64 MMU，STM32F405 `netduinoplus2` 非 MMU/可选 MPU，以及 RV32 Sv32/非 MMU。Newlib 和 musl 使用实际源码构建；三个主架构支持 2/4 核验收。完整软件验收还包括持久化、服务恢复、故障注入和独立 Lua/LVGL 应用。

- [架构与边界](docs/MICROKERNEL.md)
- [构建与验证命令](docs/QEMU_VALIDATION.md)
- [本次验证结果](docs/VALIDATION_RESULTS.md)
- [阶段 0–5 执行清单](docs/EXECUTION_PLAN.md)
- [C 库与 POSIX 支持范围](docs/POSIX_SUPPORT.md)
- [独立应用 ABI](docs/ELF_ABI.md)

目录按内核机制、架构、用户服务、运行库、应用和测试划分，见 [源码结构](docs/SOURCE_LAYOUT.md)。旧 RTOS、旧板级配置与补丁机型已清理；Lua/LVGL 源码位于 `third_party/`。

普通启动使用 `make PLATFORM=arm64_virt BUILD_MODE=normal qemu-run`，nshell 支持文件操作、目录切换、引号/转义、历史和行编辑，见 [nshell 使用说明](docs/NSHELL.md)。三个 MMU 主架构的 Linux 兼容配置已验证真实 glibc、动态链接和部分 POSIX 接口；完整 POSIX.1-2017、网络与真实板级外设仍未完成。详细范围以支持文档为准。

ESP32-S3 的 ESP-IDF 兼容移植由 NOS 管理任务/调度，提供单核共享内存基线、官方 SDK 外设依赖链接审计与实际 S3 QEMU 回归；双核、无线、USB 及完整实板生态尚未验收。构建入口为 `make PLATFORM=esp32s3 firmware`，详细依赖、自动化和边界见 [ESP32-S3](docs/ESP32_S3.md)。

ESP32-S3 的运行专项、设备管理接入和公共实现冻结验收记录见 [移植验证](docs/ESP32_S3_VALIDATION.md)。

ESP32-S3 外设通过 [NOS 设备服务模型](docs/ESP32_S3_DEVICES.md) 管理，底层复用官方 IDF 驱动与无线库；24 类服务（含自动 OTG、固定 USB Serial/JTAG）的功能子集和未实测边界见该文档。

ESP32-S3 的 GATT 服务端、USB Host HID/MSC 与自动 OTG 接口及官方 QEMU 验证方法见 [连接能力](docs/ESP32_S3_CONNECTIVITY.md)。

ESP32-C3 单核 RISC-V 移植复用同一 NOS/IDF 桥接；构建、烧录、17类受管理服务和芯片限制见 [ESP32_C3.md](docs/ESP32_C3.md)。
