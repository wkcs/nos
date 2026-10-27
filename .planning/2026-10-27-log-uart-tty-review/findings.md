# 审查证据

- 工作区初始 git status --short 为空。
- 当前存在 printk 固定 4096 字节格式缓冲、日志 kfifo、UART poll 接口；待追踪消费端和并发保护。
- LOG_FIFO_BUF_SIZE 在 STM32 defconfig 为 512 或 4096；UART_DMA 默认关闭。

## 已读实现
- printk.c：全局 4096 B 缓冲无锁、vsprintf 无边界、日志判断 level > threshold；枚举与 Kconfig 的 syslog 描述不匹配。
- log.c：char *log_buf[N] 占 N * sizeof(void*)，但 kfifo 只初始化 N 字节；非 DMA 分支丢弃 console_write 返回值。
- console.c：全局单一 console，getc/putc 可空但调用不判空。
- NK60：RX IRQ -> kfifo -> sem_get 的 console_getc；DMA 共用非原子状态与单一临时缓冲；shell 的 console_write 绕过日志 DMA。
- ZJ/WL：RX 中断内行编辑和同步 echo，console_ops 未实现 getc/putc。
- QEMU STM32：设备模型 UART1 + poll 输入输出，send_log 为空，UART_DMA=y 会进入无人消费的日志 FIFO。
- ARM64 HAL 仅定义 PL011 基址，尚未看到可用 console 驱动。

- spinlock.h 把 IRQ 保存值放入共享锁对象，在拿锁之前写；ARM64 多核争用可能覆盖 IRQ 状态，不能直接复用于 SMP 日志锁。
- ARM arch_spin_lock 在等待锁时调用 pr_err；日志路径使用同一锁时存在递归风险。
- Kconfig FIFO 范围允许非 2 次幂，但 kfifo_init 拒绝；需要配置约束或独立 ring。
- libc _write_r 对 stdout/stderr 无论 console 返回什么都报 cnt；_read_r 把 NUL 当 EOF。
- 新 UART1 device 与旧板级 uart_config 路径并存，需要检查初始化覆盖。

- NK60 RX 通知使用 uint8_t sem.count，FIFO 为 512 B；256 字节无人读取突发后计数回卷，且入队失败仍通知。sem_get 的检查与挂等待队列之间无统一原子保护。
- panic 先禁 IRQ，再走普通日志/DMA，可能只发送第一批日志；缺少独立有界 polling 路径。
- shell_printf 使用栈上 1024 B + vsprintf；nshell task 固定 8192 B 栈，与最小 20 KiB RAM 目标冲突。
- ARM64 CMake 未纳入 qemu-virt board 源码，板目录没有链接脚本，架构 entry/context 仍在 bring-up 阶段；不能声称 ARM64 运行已支持。
- QEMU STM32 UART1 配置 115200 8N1，同步 TC 逐字节等待。

## 验证结果（2026-10-27）
- 四个金标 STM32 cmake 配置+构建均 exit 0，构建日志无 warning:。
- ELF nm 验证：QEMU/NK/WL 的日志 FIFO 各 0x800=2048 B，格式缓冲 0x1000=4096 B；ZJ FIFO 0x4000=16384 B。
- 主机探针直接编译 printk.c（使用环境桩）：threshold 0..7 输出条数为 5,4,3,2,1,0,0,0。
- 同一探针在 kernel_log_write 处重入：外层 OUTER123 被覆盖为 INNER\0 23，验证全局格式缓冲污染。
- libs/vsprintf.c 的核心数值为 uint32_t / print_number(int)，%p 和 %lu 存在 64 位截断，ll、z qualifier 缺失；size=0 端点算法还会把边界扩为全地址空间。

## 官方对照资料
- https://docs.zephyrproject.org/latest/services/logging/index.html ：前端/核心/后端、延迟与最小模式、编译期过滤、panic、瞬态字符串复制。
- https://docs.zephyrproject.org/latest/hardware/peripherals/uart.html ：poll/IRQ/async 分模式与裁剪，同一设备 IRQ/async 回调互斥。
- https://www.kernel.org/doc/html/latest/driver-api/tty/tty_driver.html ：TTY port 与设备操作的边界，适合作为完整版的职责参考。

- 用真实 printk.c + libs/vsprintf.c（仅外部依赖桩、重命名 libc 符号隔离）再次复现：阈值、重入污染一致；5999 字节 %s 的 ASan 报 global-buffer-overflow，越过 4096 B log_buf。
- 用真实 libs/vsprintf.c 验证：%p 把 0x1234567887654321 输出为 0000000087654321；%lu 输出 2271560481；%llu 原样输出 %lu；size=0 仍把 guard 改写为 ABCDhello。
- ARM64 cmake configure 成功；cmake --build build/qemu-virt-arm64 exit 1，当前工具链缺少 string.h；这是本机工具链/标准库接入边界，独立于源码可见的 PL011 与链接脚本缺口。
- 两块日志数据缓冲：QEMU/NK/WL 6144 B；ZJ 20480 B。WL 链接 RAM 20 KiB，日志数据缓冲占 30%；ZJ 链接 RAM 64 KiB，占31.25%。

- QEMU 5 秒有界启动验证出现 Welcome to NOS、Welcome to NOS Shell、nos#，未观察到 Kernel panic；进程由探针 SIGTERM 结束。启动前传入 help 未被命令循环处理，因此不计为交互或突发输入验证。
- 用户已明确确认：20 KiB 也要可用；审查报告已据此将最小配置独立核算。
- libc/CMakeLists.txt 当前无条件编入 Newlib/Picolibc 端口；选择 MUSL 符号不代表完整 musl/TTY 已实现。
- notification 实现和类型位于 include/nos/ipc.h（不是 include/kernel/notification.h）；可以作为事件位图机制候选，但其 IRQ/SMP 唤醒路径仍需验证。
