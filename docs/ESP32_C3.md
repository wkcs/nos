# ESP32-C3：NOS 调度器与官方 IDF 外设

<!-- Copyright (C) 2026 NOS contributors -->

ESP32-C3 由 [NOS 启动链与 init](ESP32_ARCHITECTURE.md) 主导运行，使用单核 RV32IMC、ESP-IDF 5.4.2/Newlib 和 NOS 任务/调度器。原始 SDK `tasks.c` 不参与构建；保留 SDK RISC-V `port.c/portasm.S` 的寄存器、GCC TLS、系统定时器和中断机制，以及原始队列/定时器等兼容集合。硬件驱动和无线二进制库继续使用 IDF 原件，应用通过 NOS 设备服务访问。

这是与 S3 一样的可信共享固件地址空间，没有 MMU 进程隔离、独立 ELF/glibc、原生 NOS VFS 或完整 POSIX。C3 没有硬件 FPU；浮点运算回归使用工具链的软件浮点支持。正常固件已接入 [常驻 nshell](ESP32_NSHELL.md) 的 UART0 诊断 profile；NLP、低功耗、看门狗和实板无线互通仍需后续实现/验收。

原生系统调用和设备管理只接受可调度的任务上下文；ISR、SDK 调度暂停或 SDK 临界区内分别拒绝为 `EDEADLK` / `EBUSY`，避免在不允许切换时阻塞。

## 构建、模拟与烧录

Linux x86_64、Python3.12 环境：

```sh
python3 scripts/esp32c3.py setup
make PLATFORM=esp32c3 firmware
python3 tests/esp32c3.py

# 包含无线及其余实际外设后端和 SDK 入口的固件
make PLATFORM=esp32c3 ESP_IDF_PROFILE=ecosystem firmware

# 串口按实板修改；默认烧录正常 runtime 固件
python3 scripts/esp32c3.py flash --port /dev/ttyACM0
# 全生态正常固件
python3 scripts/esp32c3.py flash --profile ecosystem --port /dev/ttyACM0
```

默认是4 MiB Flash、CPU0、100Hz、UART0 日志（TX21/RX20），正常应用不自动启动无线或外接外设。正常 runtime 固件为 `out/esp32c3/runtime/build/nos_esp32c3.bin`；烧录应同时使用同目录下的 bootloader、分区表和 OTA 初始化数据，由 `flash_args` 给出完整地址，不要只烧录应用 BIN。实际模组容量和接线需符合上述 profile。

独立工程可执行 `eval "$(python3 scripts/esp32c3.py env)"`，添加 `ports/esp32/components`，在 `project()` 后包含 `ports/esp32/cmake/hardware.cmake` 并实现 `nos_esp_init()`，设置目标为 `esp32c3`，并使用公共默认配置及 `sdkconfig.esp32c3.defaults`。SDK 默认的硬件栈保护依赖旧 TCB 字段偏移，当前明确禁用；自行启用会在 CMake 阶段拒绝。只接受默认 UART0、无第二控制台的已审计配置，避免设备管理与 SDK 控制台争用资源。

依赖复用 S3 固定的 IDF、Python 锁和工具目录；RISC-V 工具链为 `esp-14.2.0_20241119`，官方 QEMU 为 `esp-develop-9.2.2-20260417`。RISC-V 模拟器包 SHA-256：`547f03e04701a92cbb699f7f7d015adc1f5b5ef93cbb94c0dd9b7107e2d84e77`。构建和专项保留源码身份、实际 ELF 架构、镜像哈希及命令，不修改 SDK/模拟器源码。

## 设备模型与芯片差异

公共接口继续使用 `nos_idf_device_create()` / `nos_service_start()` / `nos_service_call()`；厂商句柄留在后端。C3 共17类受管理服务，`runtime` 包含前六类，`ecosystem` 包含其余11类。各类的功能子集沿用 [设备模型接口](ESP32_S3_DEVICES.md)，C3 的差异如下。

| 服务 | C3 配置与边界 |
| --- | --- |
| GPIO、UART、SPI、I2C、NVS、OTA | UART1；SPI2；I2C0；GPIO11–17保留给 Flash/电源，GPIO20/21保留给控制台。OTA finish 验证镜像，不自动选为启动槽 |
| Wi-Fi、BLE | 官方 Wi-Fi STA 及 NimBLE GATT 服务端；使用相同 NOS 连接/值/通知/配对接口；尚未实板验证 RF |
| I2S、PWM、RMT、Timer | I2S0 TX；LEDC四组；RMT两个 TX候选通道；两个 GPTimer。请求不存在的实例返回错误 |
| ADC、Temperature | oneshot 原始计数和毫摄氏度；不表示已实现全部 ADC/DMA/校准接口 |
| SDSPI、CAN | SPI2 卡接口；经典 TWAI，需要外部收发器；不支持 CAN-FD |
| USB Serial/JTAG | SDK 固定 CDC 串口，独占 GPIO18/19 和 USB 控制器；不提供 Host、OTG 或自定义描述符 |

C3 没有 USB OTG、MCPWM、PCNT、原生 SDMMC Host 或 RGB LCD 控制器；对应服务创建返回 `ENOTSUP`，不注册虚构设备。SPI 屏幕等业务可在现有总线服务上开发，但不是已有 RGB Display 服务。

固定 USB 串口使用 `NOS_SERVICE_USB_SERIAL_JTAG`：`NOS_IO_READ/WRITE` 接受 `nos_io_buffer`，非阻塞、支持短传输，无数据/空间返回 `EAGAIN`；`NOS_IO_COMMIT` 接受微秒 `uint32_t` 超时（最多1秒，按系统 tick 向上取整），等待 SDK TX 完成。停止失败保留控制器和引脚租约，允许重试。ROM 下载和调试功能属于芯片/SDK，不能把固定 CDC 改称 TinyUSB。

C3 原生任务池16项、页池20×4KiB，包含 idle、main、esp_timer、软件定时器等系统任务；原生每任务占一页，SDK 栈另从 SDK heap 分配。此预算与 S3 的24任务/28页分开，不保证无线/蓝牙/总线全部同时满负载可用；应用仍须按实际工作集计算 SRAM。

## 验证边界

专项构建正常、runtime回归和ecosystem回归三种固件，在官方 `esp32c3` machine 实际启动，检查 NOS 调度/IPC、抢占、TLS/errno/软件浮点、任务池耗尽/回收、GPIO/UART/NVS/OTA 拒绝路径和芯片限制。完整生态链接26个实际 SDK 入口，并检查没有编译 SDK 原 `tasks.c`、Xtensa port 或 C3 不存在的 OTG 后端。

模拟器 eFuse 使用其公共序列化布局，芯片版本0.3；生态专项额外使用官方 C3 默认的出厂校准版本1.3（BLK2 bit128=1，零差值采用 SDK 参考码），让 SDK 的 ADC 构造函数走出厂数据路径。不是 ADC 转换验证，也不改写实板 eFuse。回归镜像会写 NVS/非运行 OTA 槽，必须显式 `--regression` 构建。

官方 C3 machine 没有 Wi-Fi/BLE RF 或 USB Serial/JTAG 外设模型。GATT/USB/OTG 纯软件状态检查不构成 C3 拥有 OTG 控制器的证据；USB 串口另外以实际适配代码的 SDK 故障模型验证。射频、外接总线、USB 枚举、供电、实板 DMA/故障恢复仍未实测。最新执行记录见 [ESP32_C3_VALIDATION.md](ESP32_C3_VALIDATION.md)。
