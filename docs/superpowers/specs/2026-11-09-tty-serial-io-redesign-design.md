# NOS 串口 (UART)、TTY 与进程输入输出重定向重构设计规范

- **文档状态:** 待评审 (Draft)
- **创建日期:** 2026-11-09
- **目标版本:** NOS v2.0
- **涵盖范围:** 底层串口驱动接口 (UART HAL)、控制台与日志解耦、TTY 线路规程 (Line Discipline) 与 DevFS、进程级文件描述符表 (Per-Task FDs)、`dup2` 与 C 库标准流 (stdin/stdout/stderr) 重定向、Shell 原生 I/O 重定向。

---

## 1. 背景与设计目标

### 1.1 现状与技术痛点
NOS 在近期的演进中构建了日志系统、Console 适配层和简化版 TTY，但在实际使用中暴露了严重的分层错位与架构断层：
1. **底层串口 API 职责严重倒置（“非常奇怪的串口 API”）**：
   在 `include/drivers/serial.h` 的 `struct uart_driver_api` 中，底层串口硬件驱动居然包含了 `write_record`、`write_recordv`、`write_text_recordv`、`write_room`、`get_stats` 等接口。底层硬件驱动被迫理解“日志记录（Log Record）”、“两段式向量拼接”以及“文本 `\n -> \r\n` 换行扩展”，导致 STM32 / PL011 驱动内部堆砌了大量文本扫描和丢包统计代码，破坏了驱动层的纯粹性与可移植性。
2. **TTY 孤立且未融入 VFS**：
   `nos_tty` 仅是内存中的孤立数据结构，没有接入 VFS 文件抽象，也没有设备节点（如 `/dev/ttyS0`）。只有 `nshell` 通过私有指针强行调用，其他任务无法以 POSIX 标准接口访问终端。
3. **缺乏进程级文件描述符（Per-Task FDs）**：
   `fs/vfs_fd.c` 中仅有一个全局静态数组 `open_files[16]`，任务之间缺乏文件描述符隔离，多任务并发打开文件极易冲突，无法实现标准的 `dup` / `dup2`。
4. **C 库标准流被硬编码截胡，重定向彻底失效**：
   在 `libs/libc/port/picolibc_newlib.c` 中，`fd == 0` 被硬编码调用 `console_getc()`，`fd == 1 || fd == 2` 被硬编码调用 `console_write()`，且普通文件强制加上偏移量 `NOS_LIBC_STDIO_FD_OFFSET = 3`。这导致 `printf`、`puts`、`scanf` 彻底脱离 VFS，永远无法重定向到文件或管道。
5. **Shell 重定向为特化补丁**：
   `nshell` 内部的重定向与管道采用私有结构体（`shell->output_file`、`shell_stream_pipe`）打补丁，仅对内部特定内置命令生效，后台任务或标准 C 库函数均无法享受到重定向能力。

### 1.2 重构目标
参考 Linux 等主流 POSIX 操作系统的经典分层架构，结合 NOS 作为微内核/RTOS 的资源约束（**最低保证 20 KiB RAM 的 WL-001 稳定可用**）：
1. **纯净化串口 HAL**：彻底剥离 record/text 等一切业务逻辑，UART 驱动回归纯粹的硬件控制与字节流收发；
2. **规范化 TTY 与线路规程**：实现支持 Raw / Canonical 模式及基础 Termios 标志转换的轻量 TTY，并以标准字符设备注册至 DevFS（`/dev/ttyS0`、`/dev/console`）；
3. **建立进程级文件描述符表**：在 `struct task_struct` 中内嵌紧凑轻量的 `files[]` 数组，默认 0/1/2 映射至终端控制台，支持任务退出自动资源回收；
4. **打通系统级标准 I/O 重定向**：实现 `nos_dup` 与 `nos_dup2`，移除 C 库 `picolibc_newlib.c` 的硬编码旁路，使 `printf`、`scanf` 具备原生重定向能力，驱动 `nshell` 重定向与管道彻底原生化。

---

## 2. 总体分层架构

重构后的系统遵循严密的 4 层架构：

```
+-----------------------------------------------------------------------+
|  用户态 / 进程层 (Application / Shell / Libraries)                     |
|  - printf(), puts(), scanf(), read(0), write(1)                       |
|  - Shell 原生重定向: cmd > file.txt (dup2), cmd1 | cmd2 (pipe + dup2)  |
+-----------------------------------------------------------------------+
                                  |
                                  v
+-----------------------------------------------------------------------+
|  进程级文件描述符层 (Per-Task File Descriptor Table)                     |
|  - task->files[CONFIG_TASK_MAX_FDS]: 0=stdin, 1=stdout, 2=stderr      |
|  - 系统调用契约: nos_open, nos_read, nos_write, nos_close, nos_dup2   |
|  - 父子任务文件继承与退出自动回收机制                                    |
+-----------------------------------------------------------------------+
         |                                           |
         v (常规文件 / 管道)                         v (字符设备 /dev/...)
+------------------------+                 +----------------------------+
| VFS 常规文件 / 管道    |                 | DevFS 字符设备注册层       |
| (FatFs / RamFS / Pipe) |                 | (/dev/console, /dev/ttyS0) |
+------------------------+                 +----------------------------+
                                                         |
                                                         v
+-----------------------------------------------------------------------+
|  TTY 核心与线路规程 (TTY Core & Line Discipline / LDISC)                |
|  - 模式切换: Canonical (行缓冲、退格、回显) vs Raw (字节直通)           |
|  - 标志处理: ICRNL (输入回车转换行), ONLCR (输出换行转回车换行)        |
|  - 标准接口: 统一实现 struct file_operations (open/read/write/ioctl)  |
+-----------------------------------------------------------------------+
            | (纯字节流)                                ^ (纯字节流)
            v                                           |
+-----------------------------------------------------------------------+
|  底层串口硬件抽象层 (Serial Driver / UART HAL)                        |
|  - poll_in, poll_out, read, write, configure                          |
|  - 中断 / DMA 驱动的硬件 kfifo 搬运，无任何日志记录或文本展开逻辑       |
+-----------------------------------------------------------------------+
                                  ^
                                  | 紧急 Polling 输出
+-----------------------------------------------------------------------+
|  独立内核控制台 (Kernel Console / printk / earlycon / panic)           |
|  - 专用于 printk 消息分发与崩溃 dump，直通串口底层，旁路 TTY 复杂逻辑   |
+-----------------------------------------------------------------------+
```

---

## 3. 详细设计规范

### 3.1 串口硬件抽象层（UART HAL）重构与 Console 解耦

#### 3.1.1 纯净的 `struct uart_driver_api`
在 `include/drivers/serial.h` 中彻底重写驱动 API，仅保留纯粹硬件抽象：

```c
struct uart_config {
    uint32_t baud_rate; /* 波特率: 如 115200 */
    uint8_t  data_bits; /* 数据位: 5, 6, 7, 8 */
    uint8_t  parity;    /* 校验位: UART_PARITY_NONE, ODD, EVEN */
    uint8_t  stop_bits; /* 停止位: UART_STOP_BITS_1, 2 */
    uint8_t  flow_ctrl; /* 流控: UART_FLOW_NONE, RTS_CTS */
};

struct uart_driver_api {
    /* 1. 硬件轮询收发 (早期启动 earlycon、无中断轮询、测试) */
    int (*poll_in)(const struct device *dev, unsigned char *in);
    int (*poll_out)(const struct device *dev, unsigned char out);

    /* 2. 缓冲流式收发 (驱动内部维护 RX/TX FIFO 或 DMA，中断唤醒) */
    ssize_t (*read)(const struct device *dev, unsigned char *buf, size_t len, bool wait);
    ssize_t (*write)(const struct device *dev, const unsigned char *buf, size_t len);

    /* 3. 硬件参数配置 */
    int (*configure)(const struct device *dev, const struct uart_config *cfg);
    int (*get_config)(const struct device *dev, struct uart_config *cfg);

    /* 4. 紧急/Panic 路径 (绕过锁与中断，保障崩溃日志安全排空) */
    void (*panic_begin)(const struct device *dev);
    void (*panic_write)(const struct device *dev, const unsigned char *buf, size_t len);
};
```

**废除与清理列表**：
- 删除 `write_record`、`write_recordv`、`write_text_recordv`、`write_room`、`get_stats`；
- 移除驱动内部针对单条日志的容量断言（如 `CONFIG_LOG_MESSAGE_SIZE <= CONFIG_UART_TX_BUFFER_SIZE + 1`）；
- 驱动收发统一返回实际传输字节数（正整数）或标准负数错误码（如 `-EAGAIN`, `-EINVAL`）。

#### 3.1.2 内核 Console 与 Printk 职责收敛
- **日志记录与格式化**：由 `kernel/printk/log.c` 自行维护环形缓冲与记录头；
- **控制台输出适配**：控制台适配器（`drivers/serial/console_uart.c`）注册为日志后端之一，调用通用的 `uart_write()` 写入串口底层 TX 队列；若开启 `CONFIG_CONSOLE_CRLF`，换行展开在控制台发送适配器或 TTY 输出层处理；
- **Panic 输出**：内核崩溃时直接调用 `uart_panic_write()`（若底层未提供则回退至循环调用 `uart_poll_out()`），无锁排空，无需驱动理解任何日志记录概念。

---

### 3.2 TTY 核心、线路规程与 DevFS 字符设备

#### 3.2.1 线路规程与 Termios 精简模型
在 `include/nos/tty.h` 中引入轻量 POSIX 兼容的 termios 与 TTY 对象：

```c
#define ICRNL   (1U << 0)  /* 输入: 将 CR 转换为 NL */
#define IGNCR   (1U << 1)  /* 输入: 忽略 CR */
#define ONLCR   (1U << 2)  /* 输出: 将 NL 转换为 CR-NL */
#define ICANON  (1U << 3)  /* 规范模式 (行缓冲与行编辑) */
#define ECHO    (1U << 4)  /* 回显输入字符 */
#define ECHOE   (1U << 5)  /* 退格擦除回显 (BS-Space-BS) */

#define TCGETS  0x5401     /* 获取 termios 属性 */
#define TCSETS  0x5402     /* 设置 termios 属性 */

struct nos_termios {
    uint32_t c_iflag;      /* 输入模式: ICRNL, IGNCR */
    uint32_t c_oflag;      /* 输出模式: ONLCR */
    uint32_t c_lflag;      /* 本地模式: ICANON, ECHO, ECHOE */
};

struct nos_tty {
    const struct device *dev;       /* 绑定的串口设备 (UART) */
    struct nos_termios   termios;   /* 终端配置 */
    spinlock_t           lock;
    struct nos_wait_queue read_wait; /* 行就绪或有数据唤醒队列 */

#ifdef CONFIG_TTY_CANONICAL
    uint16_t line_len;
    uint16_t line_pos;
    bool     line_complete;
    char     line_buf[CONFIG_TTY_LINE_SIZE]; /* WL-001 设 64B，普通板 128B */
#endif
};
```

#### 3.2.2 TTY 的标准 VFS 文件操作适配 (`nos_tty_fops`)
TTY 向上接入标准 `struct file_operations`：
- **`tty_read()`**：
  - 若 `ICANON` 开启：行缓冲模式。接收字符直至遇到 `\n`。支持 `\b` 和 `0x7F` 退格删除，并在 `ECHO` 开启时回显输入。整行未完成且处于阻塞模式时休眠在 `read_wait`；
  - 若 `ICANON` 关闭：Raw 模式。直接调用 `uart_read()` 返回当前可用字节；
- **`tty_write()`**：
  - 若 `ONLCR` 开启：遇到 `\n` 自动展开为 `\r\n` 输出；
  - 否则直接透传调用 `uart_write()`；
- **`tty_ioctl()`**：
  - 处理 `TCGETS` 与 `TCSETS`，允许用户程序和 Shell 使用标准 POSIX ioctl 自由切换终端 Raw/Canonical 状态。

#### 3.2.3 DevFS：轻量级设备文件系统
在 `fs/devfs.c` 中实现静态设备查找表并挂载至 `/dev`：
- 静态注册项包含：
  - `/dev/console`：主控制台（指向主活动 TTY 设备）；
  - `/dev/ttyS0`：串口 1 对应的 TTY 实例；
  - `/dev/null`：空设备（读返回 EOF，写直接丢弃）；
  - `/dev/zero`：零设备（读填充 0，写丢弃）；
- 支持通过标准 `nos_open("/dev/console", O_RDWR)` 获得标准 `struct file`。

---

### 3.3 进程级文件描述符表（Per-Task FDs）与重定向实现

#### 3.3.1 `task_struct` 独立文件表
在 `include/kernel/task.h` 的 `struct task_struct` 中内嵌紧凑数组：

```c
#ifndef CONFIG_TASK_MAX_FDS
#define CONFIG_TASK_MAX_FDS 8 /* WL-001 设为 4 (16字节)；普通 MCU 设为 8；大系统设为 16 */
#endif

struct task_struct {
    /* ... 现有调度与架构成员 ... */

    /* 独立打开文件描述符表: fd 0=stdin, 1=stdout, 2=stderr */
    struct file *files[CONFIG_TASK_MAX_FDS];

    /* ... */
};
```

#### 3.3.2 任务生命周期与文件流流转
1. **系统初始根任务**：
   在根任务创建时，自动打开 `/dev/console` 填充 `files[0]`、`files[1]`、`files[2]`；
2. **任务继承（`task_create` / `task_create_ex`）**：
   新任务创建时，拷贝父任务的 `files[]` 指针数组，并对每一个非空 `struct file` 递增引用计数（`f_refs++`）；
3. **任务退出自动回收（`task_exit` / `task_cleanup`）**：
   任务销毁时，自动关闭该任务打开的所有描述符，执行 `vfs_close()` 递减引用计数并在 0 时安全释放底对象。

#### 3.3.3 `dup` 与 `dup2` 系统调用契约
在 `fs/vfs.c` 中实现：
- `int nos_dup(int oldfd)`：
  - 检查 `oldfd` 合法；寻找当前任务中第一个可用的空槽位 `newfd`；
  - 设置 `task->files[newfd] = task->files[oldfd]`，`file->f_refs++`；返回 `newfd`；
- `int nos_dup2(int oldfd, int newfd)`：
  - 参数越界或 `oldfd` 未打开返回 `-EBADF`；
  - 若 `oldfd == newfd`，直接返回 `newfd`；
  - 若 `newfd` 已被占用，先执行关闭 `nos_close(newfd)`；
  - 设置 `task->files[newfd] = task->files[oldfd]`，递增引用计数；返回 `newfd`。

#### 3.3.4 C 库对齐与原生 Shell 重定向
1. **`libs/libc/port/picolibc_newlib.c` 瘦身**：
   - 彻底删除 `NOS_LIBC_STDIO_FD_OFFSET`；
   - 彻底删除 `_read_r` 和 `_write_r` 中对 `fd == 0, 1, 2` 的特殊旁路截胡分支；
   - 所有描述符直接调用 `nos_read(fd, ...)` 和 `nos_write(fd, ...)`。
2. **Shell 原生重定向与管道**：
   - `nshell` 命令执行重定向（如 `> out.txt`）改为标准的：
     `saved = nos_dup(1); fd = nos_open("out.txt", ...); nos_dup2(fd, 1); nos_close(fd);`
     执行命令后：`nos_dup2(saved, 1); nos_close(saved);`；
   - 管道命令 `cmd1 | cmd2` 结合内核管道描述符同样使用 `dup2` 重定向标准输入输出。

---

## 4. 资源预算与边界约束

### 4.1 20 KiB RAM 极限约束核算 (WL-001)

| 子系统模块 | 旧实现占用 | 新设计占用 | 差值与分析 |
| :--- | :--- | :--- | :--- |
| **底层串口 TX 缓冲** | 256 B (强制要求容纳整条换行展开的记录) | 128 B (流式传输，无单条原子限制) | **节省 128 B** |
| **TTY 线路规程结构** | 8 B (Shell 局部简易 TTY) | ~104 B (`nos_tty` + 64B 行缓冲，静态分配) | 增加 96 B |
| **DevFS 设备查找表** | 0 B | ~64 B (4个静态指针项，无动态 inode) | 增加 64 B |
| **Task FD 描述符表** | 0 B (全局 16 项共享) | 每任务 16 B (`CONFIG_TASK_MAX_FDS=4`，4任务共 64B) | 增加 64 B |
| **静态内存总变化** | - | - | **净开销仅 +96 B** |

**结论**：重构后的体系在 20 KiB RAM 设备上增量开销不到 100 字节，且由于消除了记录展开导致的 TX 缓冲硬性膨胀，整体内存使用更加平稳可控。

---

## 5. 验证与质量保证

### 5.1 主机回归测试 (`tests/host/`)
- `test_serial_hal`：验证 UART HAL 的流式收发、非阻塞 `-EAGAIN` 语义及参数配置；
- `test_tty_ldisc`：验证 Raw 直通模式、Canonical 行就绪、退格处理、回显及 `ICRNL/ONLCR` 转换；
- `test_devfs`：验证 `/dev/console`、`/dev/ttyS0`、`/dev/null` 的 lookup/open 契约；
- `test_task_fd_dup2`：验证进程 FD 隔离性、`nos_dup`、`nos_dup2`、子任务继承及退出释放；
- `test_redirect_libc`：验证将 FD 1 `dup2` 到普通文件后，标准 C 库 `printf` 的内容精准写入文件。

### 5.2 虚拟仿真与板级冒烟测试
- **QEMU STM32**：运行 `python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf`，验证基于新 TTY 的 `nshell` 启动、命令交互，以及 `echo "hello" > /test.txt && cat /test.txt` 真实重定向。
- **4 大预设编译回归**：
  - `cmake --preset nk60-v2 && cmake --build --preset nk60-v2`
  - `cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32`
  - `cmake --preset wl-001 && cmake --build --preset wl-001`
  - `cmake --preset zj-v3 && cmake --build --preset zj-v3`
  确保所有预设构建零警告，WL-001 内存不越界。
