# ESP32-C3 / S3 常驻 nshell

<!-- Copyright (C) 2026 NOS contributors -->

普通固件由 [NOS 自己的启动链与 init](ESP32_ARCHITECTURE.md) 初始化硬件支持库、内核和设备域后进入常驻 nshell；没有 IDF `app_main()` / `main_task` 派发。ESP profile 复用 `user/shell/parser.c` 与 `editor.c` 的实际解析、行编辑和历史记录；命令适配位于 `ports/esp32/components/nos_shell/`。

## 启动与连接

```sh
make PLATFORM=esp32c3 firmware
python3 scripts/esp32c3.py flash --port /dev/ttyACM0
# S3 对应替换为 esp32s3
```

终端使用 UART0、115200 baud：C3 TX21/RX20，S3 TX43/RX44。ROM 下载口 `/dev/ttyACM0` 可能是芯片的 USB Serial/JTAG，与这里的 UART0 不是同一个通道；连接 UART0 引脚对应的 USB-UART 转换器查看控制台。当前已审计配置不使用 USB 控制台。

实际启动依次显示 `NOS microkernel ...`、`[nos] init`、调度频率、原生对象/页数、SDK heap、设备所有权、UART0 与 nshell 状态，最后显示 `NOS SHELL READY` 和 `nos> `。这些数据取自实际 NOS/SDK 查询，不是固定演示文本。

| 命令 | 功能 |
| --- | --- |
| `help`、`version` | 命令表、架构/ABI/目标芯片和 NOS 调度器身份 |
| `uptime` | SDK 单调运行时间（微秒） |
| `stats`、`mem` | 原生任务/锁/端点/空闲页与 SDK heap，分别标注 |
| `ps` | NOS 的 IDF 任务桥接快照、任务名、优先级、状态和剩余栈估计 |
| `devices` | NOS 设备域的实际注册表快照、状态与资源持有数量 |
| `echo`、`history`、`clear` | 带引号/转义的参数、历史记录与终端清屏 |

支持方向键、行内编辑、退格、Ctrl+C、CR/LF、256字节有界输入和4条历史记录。错误引号、参数溢出或超长行报错后继续接受命令。该诊断 profile 不提供原生 NOS VFS、独立 ELF、Linux/glibc 或 NLP 终端绑定；`ls`、`cat`、`run` 等没有绑定的能力返回明确不支持，不编造目录或程序运行结果。

## UART0 的统一管理

`nos_devices/console.c` 使用原始 IDF UART 驱动，在 NOS 域中注册固件生命周期的 `system.console`，独占 UART0 控制器和两个控制台引脚。Flash/PSRAM 引脚另归 `system.flash-psram` 持有；正常启动时共有两个系统服务，应用设备注册表剩30槽。

shell 通过 `nos_idf_console_call()` 提交非阻塞 READ/WRITE，处理实际短传输和 EAGAIN；无输入时让出 CPU。SDK 的标准输出也接到同一 UART 驱动。该固件生命周期服务不能卸载后转交应用；部分启动失败保留已获取资源，初始化失败不继续启动 shell。IRQ、SDK 临界区和调度暂停时沿用既有上下文拒绝规则。

## 自动化验证

`python3 tests/esp32c3.py` 和 `python3 tests/esp32s3.py` 重新构建三种镜像。普通镜像通过真实 UART0 的 QEMU stdin/stdout 交互，逐条验证15项命令/编辑/错误恢复，检查输出属于本次请求而不是启动日志或输入回显；编译与符号审计排除 SDK 的 OS 启动、`app_main()` 和 `main_task`。回归镜像保持独立，验证 NOS 调度、IPC、设备容量/回收及生态后端。

两个芯片的 UART 模型均实际触发 SDK RX 中断、ringbuffer 和 NOS 任务切换。测试结束后 harness 主动终止模拟器；完整命令、输入字节、必需输出、源码身份与原始 UART 日志保存在专项报告中。此前不含 shell 的 `eae2305` 日志属于历史镜像，不能作为这个启动流程的证明；此前公共89组验收在41组后被中断，没有完整成功收据，不计为89/89通过。
