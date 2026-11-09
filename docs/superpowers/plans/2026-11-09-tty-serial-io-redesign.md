# NOS 串口 (UART)、TTY 与进程输入输出重定向实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 重构 NOS 底层串口 HAL、解耦 Console/Printk，建立符合 POSIX 规范的轻量 TTY 与 DevFS，并在微内核进程 `task_struct` 中建立独立文件描述符表与 `dup2` 机制，彻底打通 C 库与 Shell 的标准输入输出重定向。

**Architecture:** 
1. **底层串口 HAL 纯净化**：在 `include/drivers/serial.h` 中彻底剔除 `write_record*`、`write_text_recordv`、`write_room` 等业务接口，回归纯净字节流收发与参数配置；
2. **Console 与 Printk 职责解耦**：换行转换与崩溃日志输出回归 Console/Printk 适配层，紧急 Panic 使用无锁 `poll_out`；
3. **TTY 线路规程与 DevFS**：实现 Raw/Canonical 双模式、Termios 转换与 `nos_tty_fops`，挂载微型 `devfs` 到 `/dev` 注册 `/dev/console`、`/dev/ttyS0`、`/dev/null`；
4. **进程级 FD 表与标准流重定向**：在 `struct task_struct` 内嵌轻量 `files[]` 数组，实现 `nos_dup` / `nos_dup2`，移除 C 库 `picolibc_newlib.c` 的硬编码截胡，重构 `nshell` 基于 `dup2` 实现原生 I/O 重定向。

**Tech Stack:** C99/GNU11, NOS 微内核调度/同步原语 (`spinlock_t`, `struct nos_wait_queue`, `kfifo`), VFS/DevFS, Picolibc/Newlib, CMake/Ninja.

## Global Constraints
- 严格遵循 NOS 代码风格：4 空格缩进，禁止使用 Tab，K&R 大括号风格；
- 代码注释必须使用中文；
- 严格受控内存占用：确保在 **20 KiB RAM 的 WL-001** 上可用且链接不超标；
- 每次修改后确保四大板级预设（`nk60-v2`、`qemu-stm32`、`zj-v3`、`wl-001`）构建零错误、零新增警告，主机回归全绿。

---

### Task 1: 纯净化串口驱动接口 (`include/drivers/serial.h`) 与底层驱动瘦身

**Files:**
- Modify: `include/drivers/serial.h`
- Modify: `drivers/serial/uart_stm32.c`
- Modify: `drivers/serial/uart_pl011.c`
- Modify: `drivers/serial/uart_nuc472.c`
- Modify: `tests/host/test_serial_api.c`
- Modify: `tests/host/test_stm32_uart.c`

**Interfaces:**
- Consumes: `struct device` from `<kernel/device.h>`.
- Produces: Clean `struct uart_config` and `struct uart_driver_api` with `poll_in`, `poll_out`, `read`, `write`, `configure`, `get_config`, `panic_begin`, `panic_write`.

- [ ] **Step 1: 编写/更新主机测试 `test_serial_api.c` 表达新的纯净 UART API**

在 `tests/host/test_serial_api.c` 中：
```c
#include <drivers/serial.h>
#include <assert.h>
#include <string.h>

static unsigned char mock_tx_buf[128];
static size_t mock_tx_len = 0;

static ssize_t mock_uart_write(const struct device *dev, const unsigned char *buf, size_t len)
{
    (void)dev;
    if (len > sizeof(mock_tx_buf) - mock_tx_len) {
        len = sizeof(mock_tx_buf) - mock_tx_len;
    }
    memcpy(mock_tx_buf + mock_tx_len, buf, len);
    mock_tx_len += len;
    return (ssize_t)len;
}

static int mock_uart_poll_out(const struct device *dev, unsigned char out)
{
    (void)dev;
    if (mock_tx_len < sizeof(mock_tx_buf)) {
        mock_tx_buf[mock_tx_len++] = out;
        return 0;
    }
    return -EAGAIN;
}

static const struct uart_driver_api mock_api = {
    .write = mock_uart_write,
    .poll_out = mock_uart_poll_out,
};

static const struct device mock_dev = {
    .api = &mock_api,
};

int main(void)
{
    unsigned char test_data[] = "hello uart";
    ssize_t rc = uart_write(&mock_dev, test_data, sizeof(test_data) - 1);
    assert(rc == sizeof(test_data) - 1);
    assert(memcmp(mock_tx_buf, "hello uart", 10) == 0);
    return 0;
}
```

- [ ] **Step 2: 运行测试验证旧接口编译或逻辑失败**

Run:
```bash
python3 -m unittest tests/host/run_serial_api_test.py
```
Expected: 编译报错（因旧接口仍要求 `write_record` 等）。

- [ ] **Step 3: 重构 `include/drivers/serial.h`**

在 `include/drivers/serial.h` 中：
1. 定义 `struct uart_config`；
2. 重构 `struct uart_driver_api`，彻底移除 `write_record`、`write_recordv`、`write_text_recordv`、`write_room`、`get_stats`；
3. 更新内联帮助函数 `uart_write`、`uart_read`、`uart_poll_out`、`uart_poll_in`、`uart_configure`、`uart_panic_write`。

- [ ] **Step 4: 瘦身 `drivers/serial/uart_stm32.c` 及 PL011/NUC472 驱动**

在 `drivers/serial/uart_stm32.c` 中：
1. 移除 `uart_stm32_write_record`、`uart_stm32_write_recordv`、`uart_stm32_write_text_recordv`、`uart_stm32_write_room`、`uart_stm32_get_stats`；
2. 移除对 `CONFIG_LOG_MESSAGE_SIZE` 容纳量的强制静态断言；
3. 保留并优化 `uart_stm32_write`（纯粹压入 `tx_fifo` 触发中断/DMA）、`uart_stm32_read`（纯粹从 `rx_fifo` 提取字节）、`uart_stm32_poll_out` / `poll_in`；
4. 同步更新 `uart_pl011.c` 与 `uart_nuc472.c` 中的 API 绑定表。

- [ ] **Step 5: 运行主机回归测试验证**

Run:
```bash
python3 -m unittest tests/host/run_serial_api_test.py
python3 -m unittest tests/host/run_stm32_uart_test.py
```
Expected: PASS。

- [ ] **Step 6: 提交串口 HAL 重构**

```bash
git add include/drivers/serial.h drivers/serial/ tests/host/
git commit -m "Serial: Clean up uart_driver_api and strip log record logic"
```

---

### Task 2: 内核 Console 与 Printk 职责解耦

**Files:**
- Modify: `drivers/serial/console_uart.c`
- Modify: `include/kernel/console.h`
- Modify: `kernel/console.c`
- Modify: `kernel/printk/console_backend.c`
- Modify: `tests/host/test_console.c`
- Modify: `tests/host/test_console_newline.c`

**Interfaces:**
- Consumes: Clean `uart_write`, `uart_poll_out`, `uart_panic_write` from `<drivers/serial.h>`.
- Produces: Decoupled `console_write(const char *buf, size_t len)` and panic flush paths.

- [ ] **Step 1: 更新 Console 主机测试用例**

在 `tests/host/test_console.c` 和 `test_console_newline.c` 中：
移除对 `console_write_record`、`console_write_recordv` 的直接调用，测试直接基于 `console_write()` 验证流式 CRLF 换行展开与 Panic 应急写逻辑。

- [ ] **Step 2: 运行测试确认测试行为**

Run:
```bash
python3 -m unittest tests/host/run_console_test.py
python3 -m unittest tests/host/run_console_newline_test.py
```

- [ ] **Step 3: 重构 `drivers/serial/console_uart.c` 与 `kernel/console.c`**

1. 在 `drivers/serial/console_uart.c` 中：
   - 保持控制台适配层作为纯粹字符流适配器：当 `CONFIG_CONSOLE_CRLF` 开启时，`console_write` 逐段处理换行展开（`\n -> \r\n`），直接调用 `uart_write(console_uart, ...)`；
   - 移除 `uart_console_write_record`、`uart_console_write_recordv`；
   - Panic 路径调用 `uart_panic_write(console_uart, ...)`，若底层不支持则循环调用 `uart_poll_out`；
2. 在 `include/kernel/console.h` 与 `kernel/console.c` 中：
   - 移除 `console_write_record` 与 `console_write_recordv` 接口；
3. 在 `kernel/printk/console_backend.c` 中：
   - 后端消费日志环形缓冲后，直接调用 `console_write(buf, len)` 发送字符流。

- [ ] **Step 4: 运行测试并验证通过**

Run:
```bash
python3 -m unittest tests/host/run_console_test.py
python3 -m unittest tests/host/run_console_newline_test.py
```
Expected: PASS。

- [ ] **Step 5: 提交 Console 解耦代码**

```bash
git add drivers/serial/console_uart.c include/kernel/console.h kernel/console.c kernel/printk/console_backend.c tests/host/
git commit -m "Console: Decouple console backend from log record semantics"
```

---

### Task 3: 线路规程标准化与 TTY 子系统规范化

**Files:**
- Modify: `include/nos/tty.h`
- Modify: `kernel/tty/tty.c`
- Modify: `tests/host/test_tty.c`
- Modify: `tests/host/test_tty_canonical.c`

**Interfaces:**
- Consumes: `<drivers/serial.h>`, `<nos/wait.h>`.
- Produces: `struct nos_tty`, `struct nos_termios`, `nos_tty_init()`, `nos_tty_fops` (`open/read/write/ioctl`).

- [ ] **Step 1: 编写规范模式与 Raw 模式的 TTY 驱动测试**

在 `tests/host/test_tty.c` 与 `test_tty_canonical.c` 中：
1. 测试通过 `ioctl(TCGETS)` 与 `ioctl(TCSETS)` 切换 `ICANON`、`ECHO`、`ICRNL`、`ONLCR`；
2. 测试 Canonical 模式下退格删除字符（`\b` 与 `0x7f`）并发送擦除回显序列 `\b \b`；
3. 测试 Raw 模式下输入直接透传。

- [ ] **Step 2: 运行测试确认失败**

Run:
```bash
python3 -m unittest tests/host/run_tty_test.py
python3 -m unittest tests/host/run_tty_canonical_test.py
```

- [ ] **Step 3: 重写 `include/nos/tty.h` 与 `kernel/tty/tty.c`**

1. 在 `include/nos/tty.h` 中：
   - 定义精简的 POSIX 兼容标志位：`ICRNL`, `IGNCR`, `ONLCR`, `ICANON`, `ECHO`, `ECHOE`, `TCGETS`, `TCSETS`；
   - 定义 `struct nos_termios` 和 `struct nos_tty`；
   - 声明 `extern const struct file_operations nos_tty_fops;`；
2. 在 `kernel/tty/tty.c` 中：
   - 实现 `tty_open`、`tty_release`；
   - 实现 `tty_read`：支持 `ICANON` 模式（行缓冲并在未就绪时阻塞等待 `read_wait`）与 Raw 模式直接返回；
   - 实现 `tty_write`：支持 `ONLCR` 展开输出；
   - 实现 `tty_ioctl`：支持 `TCGETS` / `TCSETS`。

- [ ] **Step 4: 运行 TTY 测试验证全绿**

Run:
```bash
python3 -m unittest tests/host/run_tty_test.py
python3 -m unittest tests/host/run_tty_canonical_test.py
```
Expected: PASS。

- [ ] **Step 5: 提交 TTY 线路规程代码**

```bash
git add include/nos/tty.h kernel/tty/tty.c tests/host/
git commit -m "TTY: Standardize line discipline and termios ioctl interface"
```

---

### Task 4: 极简 DevFS 字符设备注册与 `/dev/` 挂载

**Files:**
- Create: `fs/devfs.c`
- Modify: `include/fs/vfs.h`
- Modify: `fs/vfs.c`
- Modify: `fs/CMakeLists.txt`
- Create: `tests/host/test_devfs.c`
- Create: `tests/host/run_devfs_test.py`

**Interfaces:**
- Consumes: `vfs_mount`, `struct file_operations`.
- Produces: `devfs_register_chrdev(name, fops, priv)`, standard `/dev/console`, `/dev/ttyS0`, `/dev/null`, `/dev/zero`.

- [ ] **Step 1: 编写 DevFS 单元测试**

在 `tests/host/test_devfs.c` 中：
1. 验证注册设备并在 DevFS 中根据名称查找；
2. 验证打开 `/dev/null`，读取返回 0 (EOF)，写入返回长度；
3. 验证打开 `/dev/zero`，读取填满 0；
4. 验证打开 `/dev/console` 返回绑定 TTY 的 `f_op`。

- [ ] **Step 2: 创建并实现 `fs/devfs.c`**

1. 实现静态查找表（预设 4~8 个设备槽位）；
2. 实现 `devfs_register_chrdev(const char *name, const struct file_operations *fops, void *priv)`；
3. 实现内置的 `/dev/null` 和 `/dev/zero` 的 `struct file_operations`；
4. 实现 `devfs_lookup` 与 `devfs_mount`。

- [ ] **Step 3: 在 `fs/vfs.c` 的 `vfs_setup_standard_mounts()` 中挂载 `/dev`**

在 `vfs_setup_standard_mounts()` 中：
1. 创建 `/dev` 目录项；
2. 挂载 `devfs` 到 `/dev`；
3. 自动将控制台 TTY 注册为 `/dev/console` 与 `/dev/ttyS0`。

- [ ] **Step 4: 运行 DevFS 主机测试**

Run:
```bash
python3 -m unittest tests/host/run_devfs_test.py
```
Expected: PASS。

- [ ] **Step 5: 提交 DevFS 代码**

```bash
git add fs/devfs.c include/fs/vfs.h fs/vfs.c fs/CMakeLists.txt tests/host/
git commit -m "VFS: Implement lightweight DevFS and register standard devices"
```

---

### Task 5: 进程级文件描述符表（Per-Task FDs）与 `dup` / `dup2` 机制

**Files:**
- Modify: `include/kernel/task.h`
- Modify: `kernel/task/task.c`
- Modify: `fs/vfs_fd.c`
- Modify: `fs/vfs_fd.h`
- Modify: `include/fs/fs.h`
- Modify: `fs/vfs.c`
- Create: `tests/host/test_task_fd_dup2.c`
- Create: `tests/host/run_task_fd_dup2_test.py`

**Interfaces:**
- Consumes: `struct task_struct`, `struct file`.
- Produces: `nos_dup(oldfd)`, `nos_dup2(oldfd, newfd)`, per-task `files[]` lifecycle (inheritance & exit cleanup).

- [ ] **Step 1: 编写 Per-Task FD 表与 `dup2` 主机测试**

在 `tests/host/test_task_fd_dup2.c` 中：
1. 测试分配 FD 属于当前任务；
2. 测试 `nos_dup(0)` 复制出新的 FD 指向同一 `file`；
3. 测试 `nos_dup2(target_fd, 1)` 将 stdout 重定向，且原先已打开的旧 1 被安全关闭；
4. 测试模拟子任务继承父任务 FD 表（`file->f_refs` 递增）；
5. 测试任务退出时遍历关闭自身的所有打开 FD。

- [ ] **Step 2: 在 `struct task_struct` 中内嵌 `files[]` 数组**

在 `include/kernel/task.h` 中：
```c
#ifndef CONFIG_TASK_MAX_FDS
#define CONFIG_TASK_MAX_FDS 8
#endif

struct task_struct {
    /* ... */
    struct file *files[CONFIG_TASK_MAX_FDS];
    /* ... */
};
```
在 `kernel/task/task.c` 的 `task_create` 中初始化继承父任务的 `files[]`，在 `task_exit` / `cleanup` 中自动调用 `vfs_close` 回收。

- [ ] **Step 3: 改造 `fs/vfs_fd.c` 从全局查找改为 Per-Task 查找**

改造 `vfs_fd_reserve()`、`vfs_fd_publish()`、`vfs_fd_get()`、`vfs_fd_close()`，所有操作均作用于 `task_current()->files[fd]`。

- [ ] **Step 4: 实现 `nos_dup` 与 `nos_dup2`**

在 `fs/vfs.c` 中：
实现 `nos_dup` 与 `nos_dup2`，处理参数合法性校验、原子更新引用计数和关闭目标冲突描述符。

- [ ] **Step 5: 运行主机测试验证**

Run:
```bash
python3 -m unittest tests/host/run_task_fd_dup2_test.py
```
Expected: PASS。

- [ ] **Step 6: 提交进程 FD 表与 `dup2`**

```bash
git add include/kernel/task.h kernel/task/task.c fs/vfs_fd.c fs/vfs_fd.h include/fs/fs.h fs/vfs.c tests/host/
git commit -m "VFS: Implement per-task file descriptor table and dup/dup2 syscalls"
```

---

### Task 6: C 库标准流彻底对齐与 Shell 原生重定向

**Files:**
- Modify: `libs/libc/port/picolibc_newlib.c`
- Modify: `app/shell/nshell.c`
- Create: `tests/host/test_redirect_libc.c`
- Create: `tests/host/run_redirect_libc_test.py`

**Interfaces:**
- Consumes: `nos_read`, `nos_write`, `nos_dup2`, `nos_open`, `nos_close`.
- Produces: Fully redirectable `printf` / `scanf` and native POSIX-style shell redirection.

- [ ] **Step 1: 编写 C 库重定向验证主机测试**

在 `tests/host/test_redirect_libc.c` 中：
1. 打开临时文件，将 FD 1 通过 `nos_dup2` 重定向到该文件；
2. 调用 libc 的 `printf("Redirection Success!\n");`；
3. 恢复 FD 1；读取临时文件，断言内容精确等于 `"Redirection Success!\n"`。

- [ ] **Step 2: 改造 `libs/libc/port/picolibc_newlib.c`**

1. 移除 `NOS_LIBC_STDIO_FD_OFFSET`；
2. 移除 `_read_r` 中 `fd == 0` 调用 `console_getc` 的特化；
3. 移除 `_write_r` 中 `fd == 1 || fd == 2` 调用 `console_write` 的特化；
4. 所有 `fd >= 0` 统一调用 `nos_read(fd, ...)` 与 `nos_write(fd, ...)`。

- [ ] **Step 3: 原生化 `app/shell/nshell.c` 中的重定向实现**

1. 移除 `shell->output_file`、`shell->error_file` 等专用补丁字段；
2. 在 Shell 执行命令循环中，对于 `>`、`>>`、`<`，直接保存标准流 `nos_dup()`，然后 `nos_dup2(target_fd, 0/1/2)`，执行完后 `nos_dup2` 还原；
3. 对于管道 `|`，使用标准管道描述符进行 `dup2` 连接。

- [ ] **Step 4: 运行 C 库重定向主机测试**

Run:
```bash
python3 -m unittest tests/host/run_redirect_libc_test.py
```
Expected: PASS。

- [ ] **Step 5: 提交 C 库标准流与 Shell 原生重定向**

```bash
git add libs/libc/port/picolibc_newlib.c app/shell/nshell.c tests/host/
git commit -m "Libc: Route standard streams through VFS and native shell redirection"
```

---

### Task 7: 全平台编译构建、QEMU 冒烟与内存核验

**Files:**
- Modify: `configs/*_defconfig` (if any default sizes adjusted)

**Interfaces:**
- Verification only.

- [ ] **Step 1: 运行全量主机回归测试**

Run:
```bash
python3 tests/run_host_regressions.py
```
Expected: 全部测试用例 PASS。

- [ ] **Step 2: 编译构建所有四大板级预设**

Run:
```bash
cmake --preset nk60-v2 && cmake --build --preset nk60-v2
cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32
cmake --preset zj-v3 && cmake --build --preset zj-v3
cmake --preset wl-001 && cmake --build --preset wl-001
```
Expected: 4 个预设均编译成功，零错误，零新增警告。

- [ ] **Step 3: 运行 QEMU STM32 冒烟与重定向端到端测试**

Run:
```bash
python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf
```
Expected: 系统启动进入 nShell 提示符，交互测试正常。

- [ ] **Step 4: 测量 WL-001 内存占用**

Run:
```bash
arm-none-eabi-size build/wl-001/nos.elf
```
Expected: 确认 `.bss` 与 `.data` 总体占用平稳，完全在 20 KiB RAM 容量限额内。

- [ ] **Step 5: 最终集成提交与清理**

```bash
git status
git commit -am "System: Complete TTY, serial HAL and I/O redirection integration"
```
