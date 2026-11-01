# NOS 日志、UART 与 TTY 落地说明

日期：2026-10-27。本文记录当前代码已经实现的功能、验证结果和仍需完成的部分。原始问题与设计依据见 [LOG_UART_TTY_REVIEW.md](LOG_UART_TTY_REVIEW.md)。

## 已实现

- 日志格式化使用每次调用独立的有界缓冲，支持 64 位整数、指针、字符串精度、编译期及运行时等级过滤。超长消息被截断并计数；时钟初始化前的时间戳为零，避免读取未初始化的 Cortex-M 定时器。
- 可选日志队列保存完整记录，记录头包含长度、等级、标志和 32 位毫秒时间戳。队列满时整条丢弃，提供丢弃、截断与容量高水位统计。时间戳按 32 位毫秒回绕。
- 日志核心将记录分发给已注册的后端（`include/kernel/log_backend.h`）；串口 console 后端默认注册并拥有自己的完整记录队列，NLK 后端拥有独立队列。各后端在其他后端拥塞时仍可接收记录。后端回调不得阻塞或递归记录日志；串口启动期和 panic 的同步路径独立于其他后端回调。
- 启动时 console 尚未就绪的日志先进入串口队列；串口队列有空位时从环形缓冲分段提交完整记录。`kernel_log_read_record` 仍消费串口队列中的一条完整记录，与串口排空竞争；它不是持久日志或多读者日志接口，NLK 不再使用它。
- 四块 STM32 板使用同一 USART1 驱动与 console 适配器。RX 和 TX 使用独立的静态队列；接收等待使用可合并通知，保留 NUL 等二进制字节；日志记录整条提交，普通写入返回实际字节数或负 errno。
- 可选 STM32 DMA TX 由同一发送队列管理，并在 DMA 传输错误时尝试转入 TX 中断发送未完成部分。panic 禁用 DMA/中断发送、等待 DMA 停止后尽力排空，并使用有界轮询输出。
- 提供 raw TTY；可选 canonical 输入提供固定行缓冲、退格、回显及回车转换。Shell 默认使用 raw 模式，继续自己处理行编辑。TTY 当前约定单读者，模式变更由调用者与读操作串行化。
- 修复 `spin_lock_irq` 在锁竞争时保存与恢复 IRQ 状态的覆盖问题，并删除 ARM 自旋锁等待路径中的递归日志调用。此修复本身不代表整个调度器已经支持 SMP。
- 增加 QEMU virt PL011 轮询设备和 console 接线，覆盖早期、基本字节收发与 panic。完整 ARM64 系统启动及 PL011 中断式收发仍未完成。

Shell 中 `log on`、`log off` 控制普通日志，`log stats` 查看 `dropped`、`truncated` 和 `high_water`。`high_water` 以字节为单位，包含记录头。UART 的 RX 队列溢出、TX 记录丢弃及 DMA 错误可通过 `uart_get_stats()` 读取；RX 计数目前统计软件队列溢出。

## 配置

| 选项 | 作用 |
| --- | --- |
| `CONFIG_LOG_BUFFERED` | 开启固定大小的完整记录队列；ARM64 QEMU virt 的轮询阶段关闭。 |
| `CONFIG_LOG_FIFO_BUF_SIZE` | 日志队列字节数，必须是 2 的幂，并能容纳一条最大消息及 8 B 记录头。 |
| `CONFIG_LOG_MESSAGE_SIZE` | 每次调用的格式化空间，包含结尾 NUL；WL-001 默认 128 B，其他 ARM 默认 256 B。 |
| `CONFIG_CONSOLE_CRLF` | 默认开启：UART 控制台将文本 LF 转为 CRLF，适合 minicom 等终端；关闭后输出 LF。原始 UART 接口不转换。 |
| `CONFIG_LOG_COMPILE_LEVEL` | 编译期最低等级；低于此等级的调用及参数求值会被移除。 |
| `CONFIG_DEFAULT_LOG_LEVEL` | 运行时最低等级，包含该等级：0=ALL、1=DEBUG、2=INFO、3=WARNING、4=ERROR、5=FATAL、6=OFF。 |
| `CONFIG_UART_RX_BUFFER_SIZE` / `CONFIG_UART_TX_BUFFER_SIZE` | STM32 接收和发送队列字节数，必须是 2 的幂；开启 CRLF 时发送队列至少为 `2 * (CONFIG_LOG_MESSAGE_SIZE - 1)`。 |
| `CONFIG_UART_BAUD_RATE` | console UART 波特率，默认 115200。 |
| `CONFIG_UART_DMA` | STM32 硬件板的 DMA 发送；QEMU STM32 板禁止此配置。 |
| `CONFIG_UART_DMA_BUFFER_SIZE` / `CONFIG_UART_DMA_STOP_TIMEOUT` | DMA 中转区大小及等待通道停止的最大循环次数。 |
| `CONFIG_UART_POLL_TIMEOUT` | 早期与异常路径等待 UART 就绪的最大循环次数。 |
| `CONFIG_TTY` / `CONFIG_TTY_CANONICAL` | 启用 TTY / 可选行模式；行模式默认关闭。 |
| `CONFIG_TTY_LINE_SIZE` | canonical 输入缓冲大小，空间不足时丢弃多余输入并保留换行。 |
| `CONFIG_SHELL_STACK_SIZE` / `CONFIG_SHELL_OUTPUT_SIZE` | Shell 任务栈及有界格式化缓冲大小。 |

`NOS_TTY_ICRNL` 将 canonical 输入回车转换为换行；`NOS_TTY_ECHO_CRLF` 只控制回显换行，不改变普通二进制写入。回显是尽力输出，当前接口不是 POSIX `termios`。需要使用 console TTY 的内核调用方可通过 `nos_console_tty()` 获取实例，然后使用 `nos_tty_set_mode()`；不要对由现有 Shell 读取的实例同时启用 canonical/echo。

日志和 Shell 内部文本使用 LF，输出到 UART console 时按 `CONFIG_CONSOLE_CRLF` 转换。Shell 的管道和文件重定向保留 LF；NLK 交互终端输出 CRLF，非交互执行输出 LF。回车键输入与输出转换独立处理。该设置由设备配置决定，并不检测 minicom；终端自身如果也开启 LF 转 CRLF，应避免重复转换。需要传输二进制数据时使用原始 `uart_write()`，而不是 console TTY 文本输出。Bootloader 的文本输出默认使用 CRLF，其原始 `uart_write()` 不转换。

日志以整条记录提交；转换后的最大长度需要两倍空间。STM32 默认 TX 队列因此在 WL-001 上从 128 B 增至 256 B，在其他 STM32 默认配置从 256 B 增至 512 B。转换直接写入 TX 队列，不使用整条展开记录的临时栈缓冲。

## 20 KiB WL-001 的静态预算

本轮默认配置 ELF（`arm-none-eabi-size`）：`.data` 1180 B，`.bss` 3208 B；链接脚本中的系统保留区共 4480 B，可供分配器使用的区域为 15488 B，早期启动及中断栈保留 512 B。数值包含对齐和其他已链接子系统，不等于应用运行后的空闲堆。

相关静态对象包括 512 B 日志记录队列、384 B `uart1_data`（含 128 B RX、128 B TX 和通知/状态对象）以及 8 B raw TTY 对象。WL-001 默认不启用 DMA、canonical TTY 或完整 Shell。日志格式化缓冲位于调用栈上，不属于上述静态对象。

GCC `-fstack-usage` 给出的单函数静态栈占用：`pr_log` 192 B、`vsnprintf` 200 B、`kernel_log_flush` 32 B、`USART1_IRQHandler` 32 B。它们不是完整调用链或嵌套中断的栈高水位；仍需在真实板和实际任务配置上测量。

## 验证

```text
python3 tests/run_host_regressions.py
cmake --preset qemu-stm32
cmake --build --preset qemu-stm32
python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf
```

本轮四块标准 STM32 预设均重新配置并构建成功，构建日志无新增报告的警告。`nk60-v2`、`zj-v3`、`wl-001` 的 DMA 配置也分别构建成功；主机模拟覆盖正常发送、满队列、DMA 错误恢复、停止等待和 panic 排空。F1 DMA IRQ14 与 F4 DMA IRQ70 的向量均核对到对应处理函数。

QEMU STM32 验证启动、Shell `help`、`log stats` 和连续输入突发；启用 canonical 功能编译后的 raw Shell 也通过相同测试。非法的日志非 2 次幂容量和小于单条日志的 UART TX 容量均在编译时被拒绝。

QEMU `netduinoplus2` 的 STM32F405 模型没有实现 DMA2；本机 QEMU 10.2 的未实现设备日志及 [QEMU STM32F405 源码](https://github.com/qemu/qemu/blob/master/hw/arm/stm32f405_soc.c) 均可确认。因此 QEMU 只验证 IRQ 模式，DMA 必须在真实硬件验证。

PL011 和格式化器已分别用 `aarch64-elf-gcc` 编译为 AArch64 目标文件；PL011 也通过模拟寄存器的主机测试。这不是 ARM64 固件运行验证。

## 尚未完成

1. **ARM64 完整闭环。** 当前本机 `aarch64-elf-gcc` 没有目标 C 库头文件，完整构建停在缺少 `string.h`。此外 qemu-virt 板仍缺固件链接脚本与完整入口、上下文切换、中断及调度实现。PL011 目前只有轮询路径，ARM64 QEMU virt 尚未启用异步日志队列。
2. **完整 POSIX 终端。** 当前 TTY 是内核内部字节流和可选简化行模式；`/dev/tty`、`termios`、`ioctl/poll`、PTY、进程组/信号、标准 FD 重定向及用户态 TTY 服务尚未实现。它们依赖现有 VFS、进程和用户态服务基础。
3. **大设备日志留存和多后端。** 当前记录队列是 console 的待发送队列，读操作会消费记录；独立留存、多读者、文件/网络后端和速率限制尚未实现。
4. **硬件与运行时验证。** STM32 DMA、故障注入、长时间压力、真实线路溢出、嵌套中断栈高水位及 20 KiB 板完整应用运行仍需要实体板验证。现有全局调度器状态也尚不能作为 SMP 完整支持的依据。
