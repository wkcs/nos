# ESP32-S3 移植验证（2026-10-24）

<!-- Copyright (C) 2026 NOS contributors -->

最新设备模型接入已完成下列冻结验证；底层继续使用原始 ESP-IDF 驱动和无线库。接口功能子集见 [设备模型接入](ESP32_S3_DEVICES.md)，无线/USB/外接外设仍未实板验收。

## 设备管理接入的最终记录

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

完整生态仍仅做链接审计，ADC 自校准未被 QEMU 模型支持。22 类后端是当前功能子集，不表示全部 SDK API、自动 OTG、BLE GATT、USB Host 类桥接、独立设备 IPC server、低功耗或实板 DMA/故障恢复已完成。

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
