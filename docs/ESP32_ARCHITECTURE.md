# ESP32：由 NOS 主导启动和运行

<!-- Copyright (C) 2026 NOS contributors -->

最新ESP32 Newlib文件/标准流专项、正常固件与原始启动日志见 [ESP32_NEWLIB_VALIDATION.md](ESP32_NEWLIB_VALIDATION.md)。下列既有验收记录保留其原始提交与范围。

ESP32-C3 / S3 使用 Espressif ROM 和二级 bootloader 加载 NOS 镜像。NOS 提供镜像入口、系统初始化顺序、内核启动及 init 任务；IDF 作为硬件支持库和第三方协议栈依赖。固件没有 IDF `app_main()`、SDK `main_task` 或 SDK OS 启动派发器。

## 启动链

```text
ESP ROM -> 原始二级 bootloader -> NOS call_start_cpu0
  -> vendor CPU HAL（BSS、向量、缓存、Flash/PSRAM、时钟）
  -> NOS start_cpu0（硬件初始化表、构造器、内核启动顺序）
  -> NOS kernel_start（页池、任务、IPC、调度器）
  -> NOS init（释放复位栈、设备模型、UART0、nshell）
```

`arch/{xtensa,riscv32}/esp_entry.S` 提供入口；C3 在调用 C 代码前配置 gp。两者都直接跳到原始 SDK CPU 硬件初始化函数，该函数通过编译定义改名为 `nos_esp_hal_cpu_init`。不在 BSS/缓存就绪前访问全局变量或输出日志。

硬件初始化回调数组的运行顺序由 `ports/esp32/startup.c` 控制。保留 SDK 表的硬件依赖顺序及 GCC 构造器 ABI；该过程不运行 SDK 的 `esp_system/startup.c` 或 FreeRTOS `app_startup.c`。硬件初始化失败和 init 意外返回均停止启动，不通过关闭 assert 隐藏失败。

`nos_boot.c` 在 NOS 内核的板级启动钩子中创建 `init`，任务记录、状态转换、优先级、超时和最终任务选择由 NOS 管理。常驻 shell 不再是某个 IDF 应用回调。回归固件使用独立的 `nos_esp_init` 实现，完成后输出明确标记并退出其任务。

## 目录职责

| 位置 | 职责 |
| --- | --- |
| `arch/xtensa/`、`arch/riscv32/` | 各 CPU 的镜像入口、上下文入口、屏障、停机和中断屏蔽 |
| `ports/esp32/startup.c`、`platform.c`、`init.c` | ESP SoC 启动策略、平台硬件钩子和系统 init |
| `ports/esp32/components/freertos/` | IDF/无线库需要的 FreeRTOS API 兼容层，由 NOS 实现任务管理；组件名是 SDK ABI 要求 |
| `ports/esp32/components/nos_devices/` | 原始 IDF 外设驱动到 NOS 设备服务的后端；应用不取得 vendor 控制器句柄 |
| `libc/port/esp32/` | Newlib 文件/标准流、目录与就绪 OS 钩子 |
| `ports/esp32/components/nos_shell/` | 共享 nshell 解析器/编辑器的 MCU 命令适配 |
| `third_party/espressif/` | 固定硬件库、协议栈、无线二进制、源码清单及许可证 |

不存在 `arch/esp_idf`。SDK 名称不构成 CPU 架构。现有 `NOS_ESP_IDF` 宏和 `nos_idf_*` API 名称保留作内部兼容接口，不能据此判断启动或调度的归属。

## 兼容层与依赖边界

IDF 无线二进制和部分原始驱动依赖 FreeRTOS 函数、Newlib TLS、事件/队列/定时器及中断 ABI。NOS 提供这些接口的任务后端；原始 `tasks.c` 不构建。保留原始 CPU `port.c/portasm.S` 的寄存器/TLS/中断保存机制与集合容器实现，这些调用 NOS 的调度入口。不能只删掉 FreeRTOS 头文件而继续声称无线库可用。

硬件支持源码、已初始化的必要子模块和无线库包含在仓库的校验归档中，构建从本地解包，不克隆外部 SDK。`scripts/espressif_sources.py` 在解包和构建前后核对归档及逐文件清单，包括可执行权限和符号链接；改动缓存或添加源码会拒绝构建。编译器、Python 构建工具和 QEMU 是宿主开发工具，仍由 setup 安装到忽略的 `out/`。

保留 Espressif Flash/IRAM/DRAM 链接布局和 DMA/capability heap 支持，用于硬件库分配；NOS 原生页池和硬件 heap 的统计仍分别列出。目前还没有统一两套分配器。默认单核、100Hz、共享固件地址空间；ESP 端的独立 ELF、完整 POSIX、SMP 和实板无线互通仍未完成。此次启动重构不代表这些能力已具备。

## 验证

运行 `python3 tests/esp32c3.py` 和 `python3 tests/esp32s3.py`。专项检查真实编译数据库、ELF 机器类型及启动符号：必须包含 NOS 启动与 init，必须排除 SDK `startup.c`、`app_startup.c`、`tasks.c` 和 `app_main/main_task`。真实 QEMU 分别执行正常启动的32项 UART nshell 交互、运行时回归和全外设链接回归。模拟器没有无线射频或完整 USB 控制器模型，不替代实板验证。

本轮干净冻结源码的结果、原始日志和正常固件见 [ESP32_BOOT_VALIDATION.md](ESP32_BOOT_VALIDATION.md)。

原生 VFS 已接入独立服务任务和端点0，nshell 文件与标准输入输出走实际 IPC；能力与限制见 [ESP32_VFS.md](ESP32_VFS.md)。[Newlib 文件与标准流](ESP32_NEWLIB.md) 已接入原生文件表，SDK 描述符保留兼容区间；分配器统一仍未完成。

原生VFS集成后的最新冻结实现为 `bfd67ab`：C3/S3八个实际镜像与公共89/89组通过，见 [ESP32_VFS_VALIDATION.md](ESP32_VFS_VALIDATION.md)。
