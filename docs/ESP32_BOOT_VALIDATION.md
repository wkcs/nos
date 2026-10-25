# ESP32 NOS 启动接管验证（2026-10-25）

<!-- Copyright (C) 2026 NOS contributors -->

被测实现为干净提交 `76a8ec89df88bb6d1ea50a318ade70bfb937401e`。两个平台 runner 和依赖完整性 runner 均实际退出0；两个平台开始/结束源码身份完全相同。源码、文档和构建输入在整个测试期间冻结。本记录在测试结束后补录，不改变被测实现。

## 实际结果

| 范围 | 结果 | 报告 |
| --- | --- | --- |
| C3 normal/runtime/ecosystem | 三次构建及真实 `esp32c3` QEMU 均通过；正常启动15项UART交互、运行时19个/生态21个必需标记，QEMU全部退出0 | `out/esp32c3/nos-owned-validation/reports/1792886406926148350/results.json` |
| S3 normal/runtime/ecosystem | 同等范围，真实 `esp32s3` Xtensa QEMU 全部通过，退出0 | `out/esp32s3/nos-owned-validation/reports/1792886405918816049/results.json` |
| 离线依赖完整性 | 9组通过：离线安装/重复安装、文件内容、权限、额外源码、链接替换、版本信息、清单/归档损坏拒绝及真实依赖核验 | `out/esp32-owned-startup/1792886404490530939/sources.json` |

平台专项同时运行现有 GATT/USB/OTG、实际 USB 适配器故障模型和实际 Wi-Fi 适配器分配/回滚/隔离/停止重试模型，并在独立工程实际配置组件管理器。所有检查通过。汇总及退出回执分别为 `out/esp32-owned-boot-evidence.json` 和 `out/esp32-owned-runner-exits.json`。

编译数据库必须包含 NOS 的 CPU 入口、startup、boot/init、调度、IPC和内存实现；必须排除 SDK `esp_system/startup.c`、FreeRTOS `app_startup.c` 和 `tasks.c`。ELF 入口地址必须等于 NOS `call_start_cpu0`；必须包含 NOS init，且没有 `app_main`、`main_task` 或 `esp_startup_start_app`。SDK 硬件 CPU 函数通过编译定义改名，源码和无线库保持原件。构建实际使用仓库解包的依赖，没有使用旧 `out/esp32-sdk/esp-idf` checkout。

实际 S3 UART 日志片段如下；C3 对应使用 `riscv32-esp32c3`。完整文件保留原始字节、bootloader输出、硬件警告、交互输入输出和 harness 结束信号，未清理或合成日志：

```text
NOS: bootloader handoff, target=esp32s3
NOS: hardware ready; starting NOS kernel
NOS microkernel xtensa-esp32s3
PASS page_allocator
NOS: init task running under NOS scheduler
NOS: init online; managed devices ready
```

原始日志：

- `out/esp32-boot-logs/76a8ec8/esp32c3-startup-nshell.log`
- `out/esp32-boot-logs/76a8ec8/esp32s3-startup-nshell.log`

## 可烧录正常固件

| 芯片 | ZIP | ZIP SHA-256 |
| --- | --- | --- |
| C3 | `out/esp32c3/artifacts/nos-esp32c3-76a8ec8-nos-boot.zip` | `f4145b309aefe0419dd35d71f10c29c0c1ad2310207dfe8999934500cc164295` |
| S3 | `out/esp32s3/artifacts/nos-esp32s3-76a8ec8-nos-boot.zip` | `86d94523cf869ab3074f4f7669e6e3add413a1eacd0fa926ff90a1b79f826ee6` |

C3 应用BIN为203264字节，SHA-256 `a33662b14daa03a1415efbd4a64f6ce593546f870a97ef0ca1ed6d5328c2e1b0`；S3为216400字节，SHA-256 `a9584add79e6c55cd12fc8660cabec62c8e1395fa0832fd8c248ee0744d72010`。

ZIP包含正常固件、原始二级bootloader、分区表、OTA初始化、ELF、烧录参数、构建身份、依赖清单、原始日志和验证报告。ZIP CRC、包内SHA256SUMS及烧录参数引用的文件均已核验。不包含自动修改外设的回归固件或QEMU专用eFuse镜像。

解压后在对应目录执行：

```sh
python -m esptool --chip esp32c3 -p PORT write_flash @flash_args
# S3 将 esp32c3 替换为 esp32s3
```

UART0为115200：C3 TX21/RX20，S3 TX43/RX44。ROM USB下载口和UART0控制台可能是不同设备，详见 [nshell连接](ESP32_NSHELL.md)。

## 范围

这轮验证证明 NOS 入口、启动阶段控制、内核调度和 init 在两个真实模拟机型运行。没有可访问实板；无线射频、USB枚举、物理Flash时序、低功耗、看门狗和多核没有新增实板结论。保留原始硬件上下文机制、协议库、硬件capability heap和NOS提供的FreeRTOS API兼容层；原生页池和硬件heap尚未统一，独立VFS/ELF和完整POSIX仍未在ESP端实现。

本轮未改公共内核/内存/运行库源码，未重新运行89组POSIX完整矩阵；不能以ESP专项替代该矩阵。此前 `eae2305` 的完整矩阵在41/89后被中断，仍没有89/89通过记录。实现边界见 [ESP32_ARCHITECTURE.md](ESP32_ARCHITECTURE.md)。
