# ESP32-S3 移植验证（最新2026-10-25）

<!-- Copyright (C) 2026 NOS contributors -->

原生VFS最新冻结实现 `bfd67ab` 已通过S3/C3各四个实际镜像及公共89/89组验收，源码身份保持一致。S3静态DRAM超预算已通过服务启动时分配文件表修复，保留24任务槽及全部已有外设后端。完整证据见 [ESP32_VFS_VALIDATION.md](ESP32_VFS_VALIDATION.md)。以下保留历史阶段记录。

最新连接能力已完成下列冻结专项验证；底层继续使用原始 ESP-IDF 驱动和无线库。接口见 [设备模型接入](ESP32_S3_DEVICES.md) 和 [GATT/Host/OTG](ESP32_S3_CONNECTIVITY.md)，无线/USB/外接外设仍未实板验收。

## NOS 自有启动入口与 init

`76a8ec8` 已在干净冻结源码上通过 C3/S3 各三种镜像与真实 QEMU。正常镜像各15项 UART 交互、运行时19/生态21个必需标记全部通过；没有 SDK OS 启动或 app_main/main_task。依赖从仓库内校验归档解包。三个 runner 实际退出0，完整记录见 [启动接管验证](ESP32_BOOT_VALIDATION.md)。

## 常驻 nshell 的历史记录

`afb0818f916e788771668121c010f8085f53d382` 在干净、冻结源码上通过 C3/S3 三种镜像专项及三个相关宿主 runner。S3 报告为 `out/esp32s3/nshell-validation/reports/1792884403539264806/results.json`：常驻normal镜像15项真实UART0命令/编辑检查通过，runtime19/生态21个必需标记齐全，全部QEMU退出0。normal不再从app_main返回；UART0由system.console独占管理，SDK驱动/RX中断/ringbuffer与NOS任务实际工作。产物、证据与范围见 [最新冻结记录](ESP32_C3_VALIDATION.md) 和 [nshell profile](ESP32_NSHELL.md)。以下保留历史快照，不能替代最新shell结果。

## GATT、USB Host 与自动 OTG 的历史记录

被测源码为干净提交 `cc005897534b3265392b27d27d9af2dce1081890`。专项期间源码、文档和构建输入冻结，开始/结束身份完全相同；下面五个 runner 均实际退出 0。本文是测试结束后的文档补录，不改变原报告身份。

| 范围 | 结果 | 报告 |
| --- | --- | --- |
| S3 固件与官方模拟器 | 三种构建/启动通过；runtime 19 个、ecosystem 20 个必需标记齐全；normal 启动及 main 返回通过 | `out/esp32s3/connectivity-validation/reports/1792877237620117254/results.json` |
| USB Host/OTG 宿主 sanitizer | 实际 HID/BOT/OTG/DWC2 代码，50000 份坏描述符与取消、故障保留等边界通过 | `out/microkernel/host-usb-host/1792877237821945725/results.json` |
| stock QEMU DWC2 | keyboard、keyboard_fs、mouse、storage 四项通过，含 BOT 读写/同步与热拔插 | `out/microkernel/qemu-usb-host/1792877236630039757/results.json` |
| 可移植设备框架 | 生命周期、资源、总线契约、NOR、16550 sanitizer 通过 | `out/microkernel/host-drivers/1792877236999766536/results.json` |
| 受管理设备服务 | 回滚、停止失败保留/重试、重入、四线程4000次调用通过 | `out/microkernel/host-managed-devices/1792877236813871097/results.json` |

证据汇总与各报告哈希在 `out/esp32s3/connectivity-evidence.json`。S3 专项还运行可移植 GATT/USB/OTG 的 ASan/UBSan/LSan 检查、实际 IDF USB/OTG 适配代码的异步 URB/板级信号故障模型，以及实际 Wi-Fi 适配代码的清理失败模型。原始 SDK 全生态后端和32个入口、NOS 调度器、应用厂商头文件限制、组件管理器2.5.2/接口3均通过审计；SDK 原 `tasks.c` 未进入固件。

官方 QEMU `esp-develop-9.2.2-20260417` 安装包 SHA-256 为 `0eecb2a34a5586c0e59110f77b9343b7b336e82fdb0e1a30e1dc1bab8a547e35`，实际二进制 SHA-256 为 `50813d894756c1448b8742e1d8db21e3cce0f4e85b56fe53e36760b941cb7f17`，已确认与包内文件相同。三次运行均为真实 `esp32s3` machine，全部必需标记及 main 返回后由 harness 主动终止，QEMU 均退出0；不是模拟开发板自行关机。

完整生态镜像使用虚拟出厂校准 eFuse：BLK2 bit128=1，零差值选用 SDK 定义的参考校准值；普通/运行镜像保留原空白校准数据。SDK、模拟器源码未打补丁。此方法使生态镜像实际启动，也执行真实 NOS OTG 空闲管理任务的创建/轮询/停止及任务栈回收检查；不验证 ADC 采样。S3 原生页池调整为28页，为 SDK GATT/bond store 留出16 KiB SRAM，原有任务耗尽/回滚和40轮回收检查仍通过。

| `cc00589` 固件 | BIN 字节数 | SHA-256 |
| --- | ---: | --- |
| normal | 171280 | `2474382f6e844d12f1f1e9386f07b5fedf1c03d2659142d6ca6b06c1091e140b` |
| runtime regression | 309776 | `2d6dbabb8a610fe850bc3a765322d9bbb65c3545aacb512c47b4cbab784c4e39` |
| ecosystem regression | 1198608 | `e87558e22a4d19a6d2abba91491ecf48ad6c58202db6f4fa18df28758f7344ec` |

正常固件归档为 `out/esp32s3/artifacts/nos-esp32s3-cc00589-normal.zip`，SHA-256 为 `96b9bf7659c7026e7caacb0e58793863c1d84d00f9892672a3437382283bc752`。包含烧录 BIN/ELF、参数、构建身份、报告及逐文件校验，已核对 CRC、烧录引用和所有文件哈希；未把会修改 NVS/OTA 槽的回归镜像作为正常固件打包。

**验证边界：** 官方 S3 machine 没有 RF 和 USB OTG 控制器模型，因此没有证明 GATT 配对互通、无线收发、S3 USB 枚举或真实供电切换。DWC2 项是宿主 C HCD 经 qtest 访问 stock raspi3b 模型，不是 S3 硬件或 NOS Raspberry Pi 启动。新增 BLE 服务端不包含 central/GATT client/OOB/HID/NLP 业务绑定；USB 类为 boot HID/MSC BOT，OTG 为经典 ID/VBUS。公共内核、架构和运行库源码本轮未修改，采用上述相关专项；没有重跑或宣称新快照通过完整89组/POSIX验收。

## 设备管理接入的历史记录

| 范围 | 冻结源码 | 结果与报告 |
| --- | --- | --- |
| 公共实现与原有平台完整验收 | `6fa7a6e91b7772ada58614b4c8e358a927621925` | **86/86 组通过**，`out/microkernel/acceptance/1792843580309736541/results.json` |
| 无线失败保护后的 S3 专项 | `4db0927f645e7f7a20144834655ffdd16267673b` | 正常启动、**16 个运行标记**、32 SDK 入口及全部设备后端链接、Wi-Fi 故障模型通过；`out/esp32s3/managed-validation/reports/1792846421120832933/results.json` |
| 快照关系与报告校验 | 公共实现完全相同 | `out/esp32s3/managed-evidence.json` |

两次测试均从干净工作区开始，结束身份相同，所有 runner 返回 0。86 组包含新增管理器资源冲突、回滚、停止失败保留、重试、重入、四线程串行化、资源池耗尽和快照测试，以及原有八平台/C 库、NK60、NLP、USB、三种 MMU 架构的 glibc 单核/四核与选定 POSIX、存储、多核、STM32/MPU、RV32 两种模式、故障和应用回归。

S3 回归将 GPIO/UART、NVS 与 OTA 改为通过 NOS 服务请求执行；新增跨总线引脚冲突、保留控制台引脚、停止后拒绝 I/O、注册表耗尽与堆回收检查。Wi-Fi 宿主模型编译实际适配代码并注入 SDK 分配/附着/初始化/停止/清理失败，确认未知残留状态不会被作为成功清理。模型不验证 RF 或 BLE 运行时。

两提交之间只有 8 个路径变化：S3 私有 `private.h`/`wireless.c`、S3 测试脚本及三个 Wi-Fi 模型文件、两份说明。已核对公共内核、架构、运行库、公共驱动头、框架、Makefile、SDK 任务桥接和参考工程完全相同。完整公共矩阵对应 `6fa7a6e`，后续 SDK 私有修复对应 `4db0927` 的 S3 复验；不冒称完整矩阵在后一个提交重新执行。本文补录不会更改原报告的源码身份；严格 `--no-build` 在文档提交后也会要求重新构建。

| `4db0927` 固件 | BIN 字节数 | SHA-256 |
| --- | ---: | --- |
| normal | 171280 | `f19dd523c1743ab6bd9be8bc6f00dc58f187841b7948cbcfa8b2ef1de5db61b5` |
| runtime regression | 300192 | `2c65967a145bddb478807da7be0bfa0ab2b9593426ba485f74da831d58057deb` |
| ecosystem linkage | 1163280 | `ff75bf77e1ed8f12ab36a4f28e42aca4b563ba488b382022f670daeeb9469673` |

正常固件归档为 `out/esp32s3/artifacts/nos-esp32s3-4db0927-normal.zip`，SHA-256 `1d9dbbbdf343f4d814ba06234eaf1b6125a129a0a0836569b1de2c5743118026`。包含完整烧录参数、BIN/ELF、两次报告、实现一致性证明和逐文件校验；CRC 和每个文件内容已核对。正常镜像不启动无线或 USB，也不是会改写外设/NVS/OTA 槽的回归镜像。

该历史快照的完整生态仅做链接审计，ADC 自校准未被 QEMU 模型支持。当时22类后端不包含自动 OTG、BLE GATT 和 USB Host 类桥接；这些能力的后续实现与验证见本文最新记录。独立设备 IPC server、低功耗和实板 DMA/故障恢复仍未验收。

## 早期调度移植记录

以下保留 `e8ca7d0` / `e85bbdd` 的历史证据；不代表设备管理接入后的最新功能状态。

NOS 调度器已在实际 `esp32s3` QEMU 机型启动并完成运行回归。公共内核完整验收为 85/85 组通过；固定 SDK 工具环境后，再次通过 S3 正常启动、14 个运行标记、32 个 SDK 入口的链接审计，以及真实组件管理器配置验证。完整物理外设生态尚未验收。

## 冻结身份与证据

| 范围 | 冻结源码提交 | 报告 |
| --- | --- | --- |
| 公共内核和原有平台完整验收 | `e8ca7d06e1f87c92ec196b2646e9fac4e2a033cd` | `out/microkernel/acceptance/1792823254327022584/results.json` |
| 最终 S3/SDK 工具专项 | `e85bbdd05c4b97b3a5bf444639f6c9384f01bbae` | `out/esp32s3/validation/reports/1792826002233142349/results.json` |
| 两次快照的实现一致性核对 | 公共实现未变化 | `out/esp32s3/final-evidence.json` |

两次执行均从干净工作区开始，结束身份相同。后一次提交只修改 S3 Python 依赖锁、环境脚本、S3 测试脚本和移植说明；`Makefile`、全部内核/架构/运行库、SDK 任务兼容层和应用源码与完整验收时逐文件一致。因此分别记录两次快照，不把它们冒称为一次覆盖所有输入的单次运行。本文与执行计划是测试结束后的结果文档更新。

## 原有完整验收

执行 `python3 tests/acceptance.py --include-posix --require-frozen-source`，85/85 个验收 runner 通过、退出 0，`source_changed_during_run=false`。包含：

- 八个平台/C 库组合，原生用户态框架、USB Host/OTG、真实 QEMU DWC2、NLP 和 NK60 的模型/固件/启动专项。
- ARMv7、ARM64、RV64 的共享文件、长路径、选定 OpenPOSIX、真实 glibc 单核/四核、持久化、普通启动和 SMP。
- 原版 `netduinoplus2` 的 STM32/MPU、RV32 MMU/非 MMU、UART pending 重使能、64 位时间边界、预期内核异常及 Lua/LVGL。

异常注入组按各自的预期故障验证；runner 退出 0 不表示故障来宾都执行了正常关机。所选 OpenPOSIX 和 glibc 用例不代表完整 POSIX.1-2017 认证。

## S3 专项

执行 `python3 tests/esp32s3.py`。三种构建均核对提交身份、编译数据库和 ELF 哈希，确认没有编译 SDK 原 `tasks.c`，NOS `select_task()`/`scheduler_tick()`/任务管理实际进入固件，SDK 不兼容 TCB 清理及被替代的任务分配实现未进入 ELF。

正常参考应用在 S3 QEMU 打印 `NOS IDF application ready` 并从 `app_main()` 返回。回归固件完成页分配器、原生系统调用/IPC、抢占、队列超时、TLS/errno/FPU 运算与删除回调、事件/流缓冲、软件和 SDK 定时器/真实 ISR 通知、优先级继承跨原生退出、静态任务/耗尽回滚、GPIO/UART/Flash/NVS、OTA 无效镜像拒绝/abort，以及 40 次任务栈回收检查，共 14 个必需标记。

测试在全部标记及 main 返回后主动终止模拟器；两个 QEMU 进程均返回 0，报告明确记录 `harness_stopped_after_markers=true`，不冒称开发板自行关机。UART1 的验证是驱动安装、空读、发送接受和卸载，没有物理串口回环证据。

SDK 全生态 profile 实际保留 32 个函数入口及其依赖，覆盖 GPIO、SPI、I2C、UART、I2S、PWM、RMT、GPTimer、MCPWM、PCNT、温度/ADC、SD/MMC、RGB LCD、USB Host、TinyUSB Device CDC/Vendor、Wi-Fi、BLE/NimBLE、网络/TLS、FAT、NVS 和 OTA。ADC 启动自校准超出当前 QEMU 模型，因此此 profile 只计构建/链接，不计启动成功。

组件管理器固定为 2.5.2，在独立临时工程真正启用 IDF 5.4 的接口版本 3，解析 IDF 版本依赖并完成 CMake 配置，核对 NOS 组件覆盖生效；缓存写入工作区。ULP FSM 汇编器版本可正常执行。环境 setup 重复执行、导出环境和 Python 包依赖检查通过；未宣称 ULP 固件运行或远端组件注册表/CI 已验证。

## 固件产物

| profile | 应用 BIN 字节数 | SHA-256 |
| --- | ---: | --- |
| normal | 168800 | `61d6f13ff90ef6620aaaf86983e71d4ae5604149cea59db24de2a91717802237` |
| runtime regression | 226608 | `d9d42308032fe69e6b0b1289d13f03eb8fb22d77816a584c15d546f85da324a6` |
| ecosystem linkage | 1015376 | `26c80d5aaa41c1875d742a29ff6826debe0cb83682394c028d0d4cf702caef1d` |

正常固件归档：`out/esp32s3/artifacts/nos-esp32s3-e85bbdd-normal.zip`，含 bootloader、分区表、OTA 初始化数据、应用 BIN/ELF、烧录参数、构建/验证身份与校验清单。归档 CRC 和逐文件 SHA-256 已核对；不包含会改写 GPIO/NVS/非运行 OTA 槽的回归应用。

默认 profile 是单核、4 MiB Flash、无 PSRAM、共享可信地址空间，不能称为 MMU 隔离。当前云环境没有可访问的 USB/ACM 实板串口；双核、无线连接/并发预算、USB 枚举及板级 OTG、外接总线/DMA/故障恢复、低功耗和看门狗仍待验证，NOS 原生 VFS/外设服务、NLP/nshell 的 S3 绑定尚未完成。具体接口与边界见 [ESP32_S3.md](ESP32_S3.md)。
