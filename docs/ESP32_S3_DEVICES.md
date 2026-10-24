# ESP32-S3 的 NOS 设备后端

<!-- Copyright (C) 2026 NOS contributors -->

硬件实现继续使用固定版本 ESP-IDF 的驱动、HAL、Wi-Fi 库、NimBLE 和 USB 栈。板级代码通过 `nos_idf_device_create()` 声明设备；业务代码使用无 SDK 类型的 `nos_service_call()` 请求。SDK 句柄仅存在于 `ports/esp_idf/components/nos_devices/`，应用不直接初始化、卸载或操作厂商驱动。

```text
NOS 应用 / 后续 NLP、nshell 服务
             ↓ NOS 请求（service_io.h）
NOS service domain：串行化、设备注册、控制器/引脚租约、生命周期
             ↓ 平台后端（nos_devices）
ESP-IDF 原始驱动 / 无线库 / TinyUSB → SDK HAL → 硬件
```

## 所有权与错误处理

公共实现位于 `servers/drivers/framework/service.c`，复用原有 `nos_device_register/bind/get/unbind` 和资源租约。控制器资源使用新增的 `NOS_RESOURCE_CONTROLLER`；引脚使用 `NOS_RESOURCE_PINCTRL`。它们只仲裁逻辑所有权，不授权 MMIO/IRQ/DMA，不替代 MMU 平台上的内核能力。

- 创建只注册停止状态的设备；显式 `nos_service_start()` 才初始化硬件。正常固件不会自动配置未知板卡引脚或启动无线/USB。
- 启动前先获取全部资源；资源冲突不会进入 SDK。部分租约失败会逆序回滚。
- SDK 启动失败会执行可重复的清理；清理失败则进入 `FAULTED`，保留对象、句柄和资源。`nos_service_stop()` 可重试；不能销毁或让另一驱动接管。
- 成功停止先回收 SDK IRQ/DMA/句柄，再断开引脚复用，最后释放 NOS 租约。公开销毁函数只有成功后才清空调用者指针。
- SDK 错误转成负 NOS 错误；后端正数状态按 EIO 处理。未实现的命令返回 ENOTSUP，停止/故障状态拒绝 I/O。
- USB Host 和 Device 使用相同控制器身份及 GPIO19/20 租约；SDSPI 与通用 SPI 使用相同总线身份。
- 默认保留 GPIO26–37 给 Flash/PSRAM、GPIO43/44 给启动控制台。这是保守的通用配置，不允许应用猜测可复用引脚。其他引脚仍需依据具体模组核对。
- 管理接口包含复制后的注册表快照，不向应用返回 SDK 控制器句柄。

IDF 后端使用同一个递归互斥量串行化管理和请求，拒绝 ISR/调度暂停上下文。后端不能同步等待另一任务回调同一域；IRQ 应排队事件。这是首个同步服务实现，长时间总线请求也会延迟域内其他请求；尚无每控制器独立工作线程和公平队列。

## 已接入的服务种类

`runtime` 构建包含 GPIO、UART、SPI、I2C、NVS 和 OTA。`ecosystem` 额外包含其余后端及自动 OTG 管理服务；共24类应用服务（含固定 USB Serial/JTAG），另有固件生命周期的 UART0 Console，新增连接接口见 [ESP32_S3_CONNECTIVITY.md](ESP32_S3_CONNECTIVITY.md)。全部后端都有实际 SDK 创建、请求和停止代码；下表同时列出当前实现的功能边界，不能据此推断 SDK 的全部 API 已有 NOS 等价接口。

| 服务 | 当前 NOS 请求与限制 |
| --- | --- |
| GPIO | 单引脚配置、读写；尚无 GPIO IRQ 订阅接口 |
| Console | 固件生命周期的 UART0、控制器与引脚租约、非阻塞 READ/WRITE；正常镜像启动 nshell，见 [启动控制台](ESP32_NSHELL.md) |
| UART | UART1/2，配置、非阻塞短读写、就绪查询；UART0 保留控制台 |
| SPI | SPI2/3，模式0–3、8位、MSB/LSB；一个片选、整消息合并，最多64字节；CPU FIFO，无 DMA |
| I2C | I2C0/1，7位、单读/单写/同地址写后 repeated START 读；不支持任意多消息、10位或自动恢复；小于一个系统 tick 的超时返回 ENOTSUP；无法区分阶段的 NACK 返回 EIO |
| NVS | u32 键读写、提交、显式清除命名空间；初始化错误不会自动擦除数据 |
| OTA | 非运行槽开始、写入、校验结束、取消；不会自动选择启动槽 |
| Wi-Fi | 官方 STA 初始化、RAM 凭据配置、连接/断开、关联状态/RSSI；连接受理不表示 DHCP 已完成；尚无 AP/扫描/事件订阅接口 |
| BLE | 官方控制器/NimBLE，GATT 服务端、特征/描述符、广播、订阅/通知/indication、MTU、配对和 bond store；central/GATT client 与 OOB 未实现 |
| USB Serial/JTAG | SDK 固定 CDC 串口，READ/WRITE 短传输与 EAGAIN、COMMIT 有界等待；不支持自定义描述符；与 OTG 共用 PHY，必须独占 |
| USB Host | SDK 枚举及 URB，复用 NOS boot HID/MSC BOT 类；支持 generation、拔插、短传输、取消失败保留、同步/恢复 |
| USB Device | TinyUSB 默认 CDC/Vendor 描述符，Vendor 短读写与卸载；尚未连接 NLP、nshell 或键盘 HID |
| USB OTG | NOS 任务自动采样明确的板级 ID/VBUS/过流输入，保护供电、去抖、放电及单角色切换；经典 OTG，不是 PD/CC |
| I2S | Master TX、Philips、16位立体声，8–96kHz；尚无 RX/TDM/PDM |
| PWM | LEDC 四组独占 timer/channel，10位，千分比占空比 |
| RMT | 官方 TX channel/copy encoder，NOS 脉冲序列；错误时保留后端副本，停止通道后释放；尚无 RX |
| Timer | GPTimer 创建/启停、64位计数读写；尚无应用闹钟事件接口 |
| MCPWM | 单组定时器、比较器、输出与占空比；尚无互补输出/死区/捕获配置 |
| Counter | PCNT 单输入上升沿计数、读数和清零 |
| ADC | 每 unit 独占一个 oneshot 通道、原始计数；尚无连续 DMA、毫伏校准接口 |
| Temperature | SDK 10–50°C 范围传感器，返回毫摄氏度 |
| SDMMC | 显式 CLK/CMD/D0、slot1、1位卡，几何查询和有界扇区读写；不格式化/挂载文件系统 |
| SDSPI | SPI2/3、显式总线引脚和 CS，卡初始化/几何/扇区读写 |
| Display | 显式12根 RGB 引脚及完整时序，8位总线 RGB565、矩形绘制；不猜测面板接线/背光供电 |
| CAN | IDF TWAI，经典 CAN、标准/扩展帧、RTR、125/250/500/1000kbps、非阻塞队列和 bus-off 状态；尚无 CAN-FD/自动恢复；需要外部收发器 |

SPI 超时通过 IDF 复位接口停止控制器，并用 SDK HAL 退还轮询事务 bookkeeping；对调用者仍返回超时，禁止故障总线继续传输，必须关闭后重新创建。没有用无限等待替代超时，也没有启用尚未验收的 SPI DMA。

NVS 由存储和无线后端在域锁下引用计数，最后一个用户停止时调用 SDK deinit。SDK 的 lwIP/default event loop 与 SDSPI 全局基础设施保留到固件结束；每个接口、卡和总线仍由各自 NOS 设备管理。启动时钟、堆、ROM、tick、日志等系统基础设施由 ESP-IDF 启动路径负责，不伪装成可卸载的应用设备。

Wi-Fi 网络接口通过原始 SDK 的可返回错误 API 创建，避免默认 STA 快捷函数在内存不足等错误上触发断言。SDK 的 Wi-Fi/NimBLE 初始化可能只返回最初错误、隐藏内部清理失败；这类失败以及不可确认完成的 Wi-Fi 清理会保留设备、NVS 引用和资源，要求重启。不能根据 NOT_INIT 或空句柄推断无线硬件已停止。

## 板级接入示例

下面引脚必须按实板确认。配置只在 BSP 中依赖 ESP32；业务使用的请求结构可在其他平台复用。

```c
#include "nos/driver/esp_idf.h"

struct nos_service *uart;
struct nos_idf_device_config board_uart = {
    .name = "board.console1", .kind = NOS_SERVICE_UART, .instance = 1,
    .pins = {17, 18, -1, -1}, .hz = 115200,
};
int error = nos_idf_device_create(&board_uart, &uart);
if (!error)
    error = nos_service_start(uart);
if (!error) {
    char hello[] = "NOS\r\n";
    struct nos_io_buffer output = {hello, sizeof(hello) - 1, 0};
    error = nos_service_call(uart, NOS_IO_WRITE, &output, sizeof(output));
    /* actual 可以小于请求长度；EAGAIN 时由服务事件循环重试。 */
}
/* 销毁返回错误时必须保留 uart，不得 free 或丢弃句柄。 */
```

新增平台实现应消费相同的 NOS 请求，持有自己的 SDK 句柄和资源 provider。新增厂商组件必须封装在后端里，不能把厂商结构、宏或句柄加入公共 `service_io.h`，也不能从应用绕过 NOS 管理器调用它。

## 验证与尚未完成的边界

```sh
python3 tests/host_managed_devices.py
python3 tests/host_driver_framework.py
python3 tests/host_esp32s3_wireless.py
python3 tests/esp32s3.py
source /workspace/.nos-env/activate.sh
python3 tests/acceptance.py --include-posix --require-frozen-source
```

宿主测试对实际管理器注入资源冲突、启动/停止失败，验证故障隔离、回滚、重试、重入拒绝、四线程串行化和快照。S3 回归经 NOS 请求测试 GPIO/UART、NVS、OTA、引脚冲突与保留引脚、停止后拒绝 I/O、注册表耗尽及堆回收。完整生态 profile 核对真实后端与 SDK 入口进入 ELF。应用源码审计拒绝直接包含外设、HAL、无线、USB、NVS/OTA 头文件。

S3 专项也运行 Wi-Fi 宿主故障模型：保留实际适配代码中的 Wi-Fi 语句，用 SDK API 替身注入分配、附着、初始化及卸载失败，同时连接实际 NOS 管理器，检查资源保留与停止重试。这不验证 RF 或 BLE 运行时；真实 Xtensa 构建负责核对官方 SDK 声明。宿主检查需要系统 GCC，以及 ASan/UBSan/LeakSanitizer 可正常运行的环境。

当前没有可访问实板。Wi-Fi/BLE、USB、外接总线、DMA/复位、电气行为与辅助外设只具有构建/链接证据，尚未实测。完整生态镜像使用 SDK 支持的出厂校准 eFuse，已经能够在官方 S3 QEMU 启动；这不验证 ADC 采样。SPI 强制复位路径也需要实板故障注入验证。

本 profile 是共享可信地址空间中的 NOS 服务接口，没有新增进程隔离或设备 IPC server。现有 MMU 平台的内核能力边界保留；后续将这些接口接入独立服务时，需要增加 IPC 请求编解码和用户缓冲区复制，不能把当前 C 指针请求直接当成 wire ABI。

尚未覆盖的 SDK 功能以 ENOTSUP 或未提供接口体现，包括 Type-C PD/CC、USB hub/UAS/等时类、BLE central/GATT client/OOB、无线网络 socket 服务、GPIO IRQ、通用 DMA 异步 API、低功耗及驱动服务异常退出后的自动回收。新增外设不能绕过设备模型补捷径。
