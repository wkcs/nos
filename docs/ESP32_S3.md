# ESP32-S3：NOS 调度器与 ESP-IDF 兼容层

<!-- Copyright (C) 2026 NOS contributors -->

当前提供 ESP32-S3 单核、共享内存的可运行移植，以及官方 SDK 外设生态的构建/链接审计。NOS 已实际接管任务管理和调度；**双核、无线收发、USB 枚举和全部物理外设尚未完成实板验收**，不能把链接通过称为“完整 ESP32 生态已验证”。

## 固定依赖与构建

依赖全部下载到忽略的 `out/esp32-sdk/`，不修改 SDK 源码，也不使用历史 `nos_stm32` QEMU 补丁。

| 依赖 | 固定版本 |
| --- | --- |
| ESP-IDF | 5.4.2，`f5c3654a1c2d2a01f7f67def7a0dc48e691f63c0` |
| Xtensa 工具链 | `esp-14.2.0_20241119` |
| Espressif QEMU | `esp-develop-9.2.2-20260417`，实际 `esp32s3` 机型 |
| esp-usb / esp_tinyusb | `68bf7f9613dbb1a31094fdb903209ae3e4bf388d`，组件 3.0.0 |
| TinyUSB 子模块 | esp-usb 固定的 `894ea01409e0407a7dbe0ee29b2d58a4691f9046` |
| Python 包 | `ports/esp_idf/python-requirements.lock`；组件管理器 2.5.2 对应 IDF 5.4 接口 |

Linux x86_64、Python 3.12 环境：

```sh
python3 scripts/esp32s3.py setup
make PLATFORM=esp32s3 firmware
python3 tests/esp32s3.py
```

其他 IDF 应用可复用环境和 NOS 组件覆盖：

```sh
eval "$(python3 scripts/esp32s3.py env)"
# 在自有工程 CMake 的 EXTRA_COMPONENT_DIRS 加入本仓库 ports/esp_idf/components
# 使用本 profile 的单核/100 Hz 配置约束，再按需开启注册表解析：
IDF_COMPONENT_MANAGER=1 idf.py -C /path/to/application build
```

参考工程默认关闭在线组件解析、使用固定 Git 组件；组件管理器 3.x 已不接受 IDF 5.4 的接口版本，因此锁定 2.5.2。自动化会在独立项目实际开启组件管理器、解析 IDF 依赖并生成 NOS 覆盖组件的编译数据库；ULP FSM 汇编器也加入 PATH，但这不代表已验证 ULP 固件或低功耗。

工具安装使用 SDK 自带校验清单，额外 QEMU 包核验 SHA-256。构建不联网取组件。脚本核对 SDK/esp-usb 的提交和本地改动；组件覆盖位于 `ports/esp_idf/components/freertos/`。云环境现有 `.nos-env` 的 Ninja、QEMU 动态库会被复用；其他 Linux 环境需提供 Ninja、Git、Python venv、libglib2.0 和 libslirp 等 QEMU 运行依赖。

正常固件位于 `out/esp32s3/runtime/build/`。默认应用只打印 NOS/芯片信息，不配置 GPIO、UART1 或写入 NVS/OTA 分区。`LIBC=idf` 使用 SDK Newlib，其他平台的 `minimal/newlib/musl` 构建路径保持独立。

```sh
# 单独构建包含全部审计入口的固件，不会自动运行无线或 USB 设备
make PLATFORM=esp32s3 ESP_IDF_PROFILE=ecosystem firmware
# 自行接入真实串口后烧录正常固件
python3 scripts/esp32s3.py flash --port /dev/ttyACM0
```

`NOS_ESP_SDK_ROOT` 可指定依赖目录，`NOS_ESP_QEMU` 可指定兼容 QEMU。审计使用固定依赖，替换 QEMU 的版本会记录在结果中。

## 调度与运行时

- `kernel/sched/{task,scheduler}.c` 管理真实 NOS 任务；SDK 原 `tasks.c` 不编译。`vTaskSwitchContext()` 调用 NOS `select_task()`，SDK systick 推进 NOS `scheduler_tick()`。
- 保留官方 Xtensa `port.c/portasm.S` 的寄存器窗口、FPU、GCC TLS 和中断上下文机制；首两个侧记录字段及 CPSA 偏移有静态断言，汇编读取的偏移常量放在 DRAM。
- 保留 SDK 的 `list.c`、`queue.c`、`timers.c`、`event_groups.c`、`stream_buffer.c` 和集合分配辅助代码。这是 FreeRTOS API/集合兼容代码，**并未清除全部 FreeRTOS 衍生代码**；原任务调度器及不兼容 TCB 清理代码不得进入最终 ELF。
- 动态/静态任务、任务通知及 ISR 唤醒、队列、事件组、流缓冲、递归锁、优先级继承、软件定时器、SDK 高精度定时器、TLS/errno 和任务删除接入 NOS。SDK 锁的继承优先级与原生 NOS IPC/锁捐赠分别记录，原生退出重算不会清除 SDK 的继承。
- 每个任务有独立 Newlib `_reent`、GCC TLS 与删除回调。运行中的任务先退出，再由 idle 释放其栈；非当前任务可立即清理。静态栈归调用者所有。池耗尽返回失败并回滚，未伪造成功。

首个 profile 强制 CPU0、100 Hz；请求 CPU1 创建任务会失败。双核 SMP、tickless idle、运行时间统计、自定义 SDK TCB 删除钩子尚未移植，相关不兼容配置会在构建时报错。默认关闭 SDK 看门狗；其运行验收不在本次基线中。SDK GDB 中对原 FreeRTOS 私有 TCB/任务链表布局的假设不能直接套用 NOS 记录。

## 内存与权限边界

ESP32-S3 在本移植中采用共享、可信固件地址空间，没有 MMU 进程隔离。SDK 驱动通过其原有 MMIO、中断分配和 DMA 机制工作；不能把这描述为已完成用户态外设能力隔离。

原生 NOS 调用只允许正常运行的任务上下文；ISR、启动前或 SDK 调度暂停期间返回 `EDEADLK`，避免在不允许切换的上下文阻塞。原生 NOS 调用通过 `nos_xtensa_syscall()` 进入同一个内核分发和能力检查路径。用户复制允许 SDK 可按字节访问的 RAM、RTC 慢速 RAM 和只读 DROM；拒绝 MMIO、空指针和只读区写入。SDK 地址域本身共享，范围检查不会隔离应用与内核数据。

当前原生 `arch_device_operation()` / `arch_irq_bind()` 返回 `ENOTSUP`；S3 的外设服务应调用 SDK 驱动。已有 NOS 统一外设框架、NLP、nshell 和 NK60 服务还需要 S3 服务绑定，不能从 SDK 驱动链接审计推断它们已经完成移植。此 profile 使用 SDK VFS/Newlib；不启动原生 NOS VFS/init，也不提供 S3 上的 glibc、独立 ELF 进程或虚拟内存。

任务池为 24 项，包含 idle、SDK esp_timer、软件定时器和 main；原生页池 32 × 4 KiB。每项原生任务内存之外另有 SDK 栈，应用必须为无线、USB 和缓冲区计算 RAM 预算。PSRAM、睡眠唤醒和多核需要指定模组后单独配置/验收。

## 外设生态的验证层级

`ecosystem.c` 通过实际保存函数地址阻止链接器回收入口和依赖。审计核对 32 个入口、编译数据库、ELF 符号与固件哈希，不仅检查头文件存在。

| 范围 | 当前证据 | 待验收 |
| --- | --- | --- |
| GPIO、UART、SPI Flash/NVS | QEMU GPIO 配置、UART1 驱动安装/读写/卸载、NVS 写入/提交/读回/擦除 | 实际引脚、电气行为、串口收发 |
| SDK 定时器/IRQ、同步 | QEMU 实际定时器 ISR 唤醒；抢占、队列超时、锁、通知、事件组、流缓冲 | 长时间负载、全部 IRQ/DMA 组合 |
| SPI、I2C、I2S、PWM、RMT、GPTimer、MCPWM、PCNT、ADC、温度传感器 | SDK 构建与入口依赖链接 | 接线、DMA/超时和故障恢复 |
| SDMMC、SDSPI、RGB LCD、FAT、TCP/IP/TLS | SDK 构建与链接 | 卡、面板、网络端到端 |
| Wi-Fi、BLE/NimBLE | 官方库及 OS 适配依赖完成链接 | 初始化、并发负载、连接、吞吐、功耗 |
| USB Host | SDK host stack 实际链接 | S3 PHY、VBUS、枚举、拔插及类驱动 |
| USB Device | 官方 esp_tinyusb/TinyUSB DWC2、CDC、Vendor 构建和链接 | 描述符、主机枚举、传输、拔插 |
| OTA | 双槽布局、QEMU inactive-slot 擦除、错误镜像拒绝及 abort | 有效镜像升级、切换启动、断电回滚 |

USB Host 与 Device 共用 S3 OTG PHY，不能同时安装。自动角色切换还需要把板级 ID/VBUS/供电与已有 NOS OTG 策略连接；当前没有 S3 的已验收角色切换 BSP，也不会默认驱动未知 VBUS 电源 GPIO。CDC/Vendor 是 SDK 接入面，不代表已经绑定 NLP/nshell。

当前全外设链接 profile 会包含 ADC 启动自校准；所选 QEMU 不产生该校准完成事件，会停在 SDK ADC 校准等待。因此只对运行 profile 执行 QEMU，完整 profile 只做编译/链接审计，不篡改 SDK 校准逻辑以伪造通过。

## 自动化与实板复验

`tests/esp32s3.py` 使用独立 `out/esp32s3/validation/`，构建正常应用和两种回归固件；运行正常应用与 runtime 回归，仅对完整生态固件做链接审计。回归应用在 `tests/guest/esp32s3_regression.c`，与正常应用分开；构建时必须显式指定 `--regression`。

覆盖原生系统调用/IPC、无主动 yield 的抢占、队列超时、两任务 TLS/errno/FPU、删除回调、软件/SDK 定时器与真实 ISR、优先级继承跨任务退出、静态任务、任务池耗尽回滚、40 次栈回收、GPIO/UART/NVS 和 OTA 无效镜像拒绝。测试要求所有标记、main 返回和 QEMU 退出状态正确；标准输入保持打开，避免 UART 后端 EOF 丢日志。结果记录提交身份、冻结源码检查、工具版本、命令和镜像哈希。构建清单绑定源码身份和 ELF 哈希，`--no-build` 也会拒绝旧快照或被替换的镜像。

当前没有可访问实板。拿到 S3 开发板后可构建并烧录专用回归应用，在 115200 波特率串口检查相同标记：

```sh
python3 scripts/esp32s3.py flash --regression \
  --output out/esp32s3/board-regression --port /dev/ttyACM0
```

该回归会配置 GPIO4、UART1，改写并清理 `nos-test` NVS 命名空间，擦除非运行 OTA 槽的一个扇区并测试拒绝错误镜像；应使用开发用分区布局。4 MiB 布局包含 24 KiB NVS、OTA 元数据、两个 1.5 MiB 应用槽及预留存储，后续生产固件应按实际模组容量、升级和文件系统策略配置。

实板后续优先级：双核上下文与跨核唤醒 → Wi-Fi/BLE 同时运行及内存预算 → USB Device/Host 与板级 OTG → 外接总线/DMA 错误恢复 → 电源管理/看门狗 → NOS 外设服务、NLP/nshell 绑定。每项保留独立的运行证据，不能用本次 QEMU 通过替代。
