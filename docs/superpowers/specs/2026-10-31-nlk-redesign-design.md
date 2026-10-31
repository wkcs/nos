# NOS 与 NLK (NOS Link Kit) 核心能力重构设计文档

- **日期**：2026-10-31
- **涉及仓库与目录**：
  - NOS 内核与服务：`/Volumes/nick-disk/projects/nos`
  - NK60-V2 独立 Bootloader：`/Volumes/nick-disk/projects/nos/bootloader`
  - NLK 主机端命令行与 SDK：`/Volumes/nick-disk/projects/nk60/nlk_cli`

---

## 1. 背景与设计目标

NOS 未来的演进目标是成为跨架构（`arm`、`arm64`、`riscv`、`x86_64`）、支持内核与用户空间隔离的下一代操作系统。原有的 NLK 实现存在以下痛点：
1. **Shell 耦合严重**：`app/shell/nshell.c` 通过 `if (shell->remote)` 硬编码区分本地 UART 与远端 NLK，无法作为统一的多前端服务演进；主机端 `nlk_cli` 仅支持单次 `exec`，缺乏像 `adb shell` 一样的交互式/非交互式统一体验。
2. **日志流不完整**：内核 `printk` 被全局 `g_log_level` 截断，且 `nlk log` 仅抓取 0.5 秒快照即退出，无法像 `adb logcat` 一样持续实时流式输出。
3. **固件升级 (`fw-update`) 绑定特定板级与外部 SPI Flash**：原协议写死 4 个无名槽位且强制前置 16B 私有头，在 `nk60-v2` 上必须绕道外部 W25QXX SPI Flash 中转。需要升级为通用 NLK 分区能力 (`nlk flash`)，由各板级向 NLK 注册分区，并支持在 STM32 上直接刷写片上内部 Flash。
4. **Reboot Flag 混乱**：`include/kernel/cpu.h`、`include/kernel/boot.h` 与 `bootloader` 存在三套冲突定义，且 `REBOOT_TO_BOOTLOADER = 0` 与冷上电复位值 `0x00000000` 冲突，无法可靠停留在 Bootloader。
5. **缺乏远端 VFS 文件管理与传输能力**：NOS 侧缺少 `NLK_CAP_VFS` 服务端实现，不支持分块大文件传输（`nlk push`/`nlk pull`）及目录枚举（`nlk ls` / 文件管理器支持）。
6. **缺乏多设备选择能力**：主机连接多个 NLK 设备时无法像 `adb -s <device>` 一样指定目标设备。

---

## 2. 跨平台与 32/64 位 UAPI 设计准则

1. **零歧义定宽类型**：`include/uapi/nos/nlk.h` 与 `nlk_cli/include/nlk/uapi.hpp` 中所有协议结构体严禁使用 `int`、`long`、`size_t`、`off_t`、`void *` 或 C `enum`；一律使用显式定宽小端整数（`uint8_t`、`uint16_t`、`uint32_t`、`uint64_t`）并显式标注 `__attribute__((packed))`。
2. **能力位图强校验 (`caps_bitmap`)**：
   - 每个 NLK 节点在 `NLK_CORE_OP_DISCOVER` 与 `NLK_CORE_OP_QUERY_CAPS` 中精确上报自身已注册的 `caps_bitmap`。
   - 主机端 `nlk_cli` 在执行子命令前检查目标节点的 `caps_bitmap`（同时设备端分发器对未注册 Capability 返回 `NLK_STATUS_ERR_NO_CAP`）：
     - `nlk shell` 要求 `NLK_CAP_SHELL`
     - `nlk log` 要求 `NLK_CAP_LOG`
     - `nlk flash` 要求 `NLK_CAP_FLASH`
     - `nlk push`、`nlk pull`、`nlk ls` 要求 `NLK_CAP_VFS`（若设备不支持 `NLK_CAP_VFS`，立即返回明确错误提示）。
3. **废弃旧脚本**：移除 `scripts/nlk_tool.py`，所有主机侧交互与自动化测试统一收敛至 C++20 项目 `/Volumes/nick-disk/projects/nk60/nlk_cli`。

---

## 3. 模块详细设计

### 3.1 内核级 Reboot Flag 与 `reboot` 命令设计

`reboot` 与启动模式标志属于 **NOS 内核原生能力**，不绑定 NLK：

1. **统一内核 Boot Flag（`include/kernel/boot.h` & `kernel/boot.c`）**：
   - 移除 `include/kernel/cpu.h` 中冲突的 `enum cpu_boot_flag`（`REBOOT_TO_*`），统一包含并使用 `<kernel/boot.h>`：
     ```c
     #define BOOT_FLAG_MAGIC         0x20300000UL
     #define BOOT_FLAG_MAGIC_MASK    0xFFFF0000UL
     #define BOOT_FLAG_MASK          0x0000FFFFUL

     #define get_boot_flag(data)       (BOOT_FLAG_MASK & (data))
     #define get_boot_flag_magic(data) (BOOT_FLAG_MAGIC_MASK & (data))
     #define get_boot_flag_data(flag)  ((BOOT_FLAG_MASK & (flag)) | BOOT_FLAG_MAGIC)

     enum boot_flag {
         BOOT_NORMAL     = 0,
         BOOT_BOOTLOADER = 1,
         BOOT_RECOVERY   = 2,
     };
     ```
   - `write_boot_flag(u32 flag)`：将传入的 `enum boot_flag` 编码为 `get_boot_flag_data(flag)`（即 `0x20300000UL | (flag & 0xFFFFU)`）后调用 `board_write_boot_flag()` 写入硬件持久寄存器（如 `RTC_BKP_DR0`）。
   - `read_boot_flag(void)`：读取 `board_read_boot_flag()`，校验 `get_boot_flag_magic(raw) == BOOT_FLAG_MAGIC`；若校验通过则返回 `get_boot_flag(raw)`，否则（如冷上电复位值 `0x00000000`）返回 `BOOT_NORMAL`。
2. **设备端 nShell `reboot` 内置命令（`app/shell/nshell.c`）**：
   - 语法：`reboot [normal|bootloader|recovery]`（默认 `normal`）。
   - 行为：输出提示日志后直接调用内核 `cpu_reboot(BOOT_*)`。
3. **NLK 远端重启转发（`NLK_CORE_OP_REBOOT = 0x04`）**：
   - 在 `NLK_CAP_CORE` 中用 `NLK_CORE_OP_REBOOT` 取代原有的 `NLK_CORE_OP_RESET`。
   - 请求载荷：
     ```c
     struct nlk_core_reboot_req {
         uint32_t boot_flag; /* 对应内核 enum boot_flag: 0=normal, 1=bootloader, 2=recovery */
     } __attribute__((packed));
     ```
   - 节点收到请求后先发送 `NLK_MSG_RESP (NLK_STATUS_OK)`，随后通过 50ms 延迟定时器调用内核 `cpu_reboot(req->boot_flag)`（在裸机 Bootloader 中则在发送 USB ACK 后写入 `RTC_BKP0R` 并触发 `SCB_AIRCR` 系统复位）。
   - 主机端命令：`nlk reboot [normal|bootloader|recovery]`。

---

### 3.2 UART 与 NLK 共用 Shell 服务架构 + `nlk shell`

1. **统一 `nos_tty` 前端抽象（`include/nos/tty.h` & `app/shell/nshell.c`）**：
   - 扩展 `struct nos_tty_ops`，增加可选的会话轮询/控制钩子：
     - `bool (*check_hangup)(struct nos_tty *tty)`：检查远端会话是否重连或主动断开（用于清空残留行缓冲并打印新会话欢迎横幅）。
     - `void (*notify_exit)(struct nos_tty *tty, int32_t exit_code)`：当用户在 Shell 中执行 `exit` 时通知前端（对于 UART 前端仅重置会话并重新打印提示符；对于 NLK 前端则发送 `NLK_SHELL_STREAM_EXIT` 帧并关闭当前远端会话）。
   - **UART Console 前端 (`kernel/tty/tty.c`)**：`nos_console_tty()` 的 `read` 对接 `console_getc()`，`write` 对接 `console_write()`。
   - **NLK Shell 前端 (`app/server/nlk/caps/cap_shell.c`)**：`g_nlk_shell_tty_ops` 的 `read` 从 `cap_shell` 的 RX 环形队列读取，`write` 向 `cap_shell` 发送 `NLK_SHELL_STREAM_STDOUT` 流帧。
   - **`app/shell/nshell.c` 彻底去特判化**：
     - 删除 `struct nshell_session` 中的 `bool remote` 字段及所有 `if (shell->remote)` 分支。
     - `nshell_init()` 启动绑定 `nos_console_tty()` 的 `"nshell_uart"` 任务，并在开启 `CONFIG_NLK_SERVER` 时启动绑定 `nlk_cap_shell_tty()` 的 `"nshell_nlk"` 任务。两个前端运行完全相同的 `nshell_task_entry` 循环，共用全部命令表与会话解析器，且互不干扰。
2. **`nlk shell` 交互式与非交互式支持（`nlk_cli`）**：
   - 移除 `nlk exec` 子命令，统一为 `nlk shell [command...]`：
     - **非交互模式 (`nlk shell <cmd> [args...]`)**：将参数以空格拼接为单行命令，调用 `NLK_SHELL_OP_EXEC` 发送给设备，流式打印 `NLK_SHELL_STREAM_STDOUT` 直到收到 `NLK_SHELL_STREAM_EXIT`，并以远端返回的 `exit_code` 作为进程退出码。
     - **交互模式 (`nlk shell` 无额外命令参数)**：
       1. 调用 `NLK_SHELL_OP_OPEN` 打开会话；
       2. 使用跨平台终端控制（POSIX `tcgetattr`/`cfmakeraw`，Windows `SetConsoleMode`）将本地终端设为 Raw 模式；
       3. 启动接收线程持续读取 `NLK_SHELL_STREAM_STDOUT` 实时写入主机 `stdout`；主线程读取本地按键通过 `NLK_SHELL_OP_STDIN` 发送给设备（支持方向键、Tab 补全、退格、`Ctrl+C` 发送 `0x03`）；
       4. 当收到设备端 `NLK_SHELL_STREAM_EXIT`（如用户在 Shell 内输入 `exit`）或本地按下 `Ctrl+]` / EOF 时，调用 `NLK_SHELL_OP_CLOSE` 并恢复主机终端属性。

---

### 3.3 `nlk log` 实时流式日志

1. **内核日志发布与控制台解耦（`kernel/printk/printk.c` & `app/server/nlk/caps/cap_log.c`）**：
   - 在 `printk.c` 中，串口控制台直接输出受 `g_log_level` 控制，但 `log_publish(level, log_buf, len)` 始终向所有已注册的 `kernel_log_backend` 分发日志记录。
   - `cap_log.c` 根据自身 `ctx->subscribed` 与 `ctx->min_level` 决定是否将日志推入 `tap_buf`，并在 `nlk_server` 主循环中通过 `nlk_cap_log_flush_pending()` 实时推送 `NLK_LOG_STREAM_RECORD` 帧。
2. **主机端 `nlk log` 持续流式打印（`nlk_cli`）**：
   - 命令语法：`nlk log [-l <min_level>] [-d <duration_sec>]`（默认 `min_level = 0` 即打印全部级别，`duration_sec = 0` 即无限持续打印）。
   - 执行流程：
     1. 检查设备是否支持 `NLK_CAP_LOG`；
     2. 发送 `NLK_LOG_OP_SET_LEVEL` 与 `NLK_LOG_OP_SUBSCRIBE(enable=1)`；
     3. 注册 `SIGINT`/`SIGTERM` 信号处理器，在循环中持续接收 `NLK_LOG_STREAM_RECORD` 并实时打印到终端（带毫秒时间戳、级别标签与序号，并立即 `fflush(stdout)`）；
     4. 当用户按下 `Ctrl+C` 或到达 `-d` 指定时长后，自动发送 `NLK_LOG_OP_SUBSCRIBE(enable=0)` 取消订阅并退出。

---

### 3.4 `NLK_CAP_FLASH` 通用分区刷写能力与 STM32 内部 Flash 直刷

`flash` 功能定义为 **NLK 协议能力 (`NLK_CAP_FLASH = 0x0002U`)**，各板级（及独立 Bootloader）向 NLK 节点注册分区表，不在 NOS 内核中引入任何分区管理层。

1. **UAPI 协议定义（`include/uapi/nos/nlk.h`）**：
   ```c
   /* NLK_CAP_FLASH (0x0002U) Opcodes */
   #define NLK_FLASH_OP_QUERY_PARTS        0x01U
   #define NLK_FLASH_OP_PREPARE            0x02U
   #define NLK_FLASH_OP_WRITE              0x03U
   #define NLK_FLASH_OP_COMMIT             0x04U

   /* 分区标志位 */
   #define NLK_FLASH_PART_F_WRITABLE       (1U << 0) /* 允许刷写 */
   #define NLK_FLASH_PART_F_NEEDS_BL       (1U << 1) /* 当前处于运行态 XIP，需先重启到 Bootloader 才能刷写 */
   #define NLK_FLASH_PART_F_REBOOT_AFTER   (1U << 2) /* 刷写提交后需重启生效 */

   #define NLK_FLASH_MAX_PARTS             8U
   #define NLK_FLASH_PART_NAME_MAX         16U

   struct nlk_flash_part_desc {
       uint8_t  part_id;
       uint8_t  flags;
       uint16_t reserved;
       uint32_t max_size;
       uint32_t erase_size;
       char     name[NLK_FLASH_PART_NAME_MAX];
   } __attribute__((packed)); /* 28 bytes */

   struct nlk_flash_query_resp {
       uint8_t  part_count;
       uint8_t  run_mode;      /* 0 = BOOT_NORMAL (OS/App), 1 = BOOT_BOOTLOADER */
       uint16_t reserved;
       struct nlk_flash_part_desc parts[NLK_FLASH_MAX_PARTS];
   } __attribute__((packed));

   struct nlk_flash_prepare_req {
       uint8_t  part_id;
       uint8_t  reserved[3];
       uint32_t total_size;
       uint32_t expected_crc32;
   } __attribute__((packed));

   struct nlk_flash_write_req {
       uint8_t  part_id;
       uint8_t  reserved[3];
       uint32_t offset;
       /* 紧随其后为原始镜像数据块 uint8_t data[] */
   } __attribute__((packed));

   struct nlk_flash_commit_req {
       uint8_t  part_id;
       uint8_t  reserved[3];
       uint32_t total_size;
       uint32_t expected_crc32;
   } __attribute__((packed));
   ```
2. **NLK 服务端分区注册接口（`include/nos/nlk_caps.h` & `app/server/nlk/caps/cap_flash.c`）**：
   - 取代原有的 `cap_fw_update.c`。
   - 板级通过 `nlk_cap_flash_register_part(struct nlk_flash_ctx *ctx, const struct nlk_flash_part_ops *part)` 注册分区（名称、`max_size`、`erase_size`、`flags` 及 `prepare`/`write`/`commit` 回调）。
   - 当对带有 `NLK_FLASH_PART_F_NEEDS_BL` 标志的分区发起 `PREPARE` 时，`cap_flash` 返回 `NLK_STATUS_ERR_BUSY`（或状态提示需进入 Bootloader）。
3. **`nk60-v2` 与 `bootloader` 直接刷写 STM32 内部 Flash 实现**：
   - **NOS 运行态 (`board/nk60-v2/nk60_nlk.c`)** 注册 3 个分区：
     1. `"bootloader"` (`max_size = 16KB`, `erase_size = 16KB`, `flags = WRITABLE`)：
        - `prepare`：校验 `total_size <= 16KB`；
        - `write`：在 `offset == 0` 时校验 Cortex-M4 向量表合法性（MSP 在 SRAM 范围，Reset_Handler 在 `0x08000000..0x08004000` 且 Thumb 位为 1），并擦除 STM32 内部 Flash Sector 0 (`0x08000000`)；随后将每包数据直接按 32-bit 字写入 `0x08000000 + offset`；
        - `commit`：刷新 STM32 ART 指令/数据缓存，直接对 `0x08000000` 处的 `total_size` 字节计算 CRC32 并与 `expected_crc32` 比对。**全程不经过外部 SPI Flash**。
     2. `"kernel"` (`max_size = 240KB`, `erase_size = 16KB`, `flags = WRITABLE | NEEDS_BL | REBOOT_AFTER`)：
        - 暴露分区描述信息，指示主机刷写该分区时需切入 Bootloader。
     3. `"assets"` (`max_size = 736KB`, `erase_size = 4KB`, `flags = WRITABLE`)：
        - 直接擦写外部 W25QXX `0x048000` 起始的 FatFs 资源分区，并在 `commit` 校验通过后调用 `nk60_assets_reload()`。
   - **独立 Bootloader (`bootloader/main.c`)**：
     - 驻留于 STM32F401 Sector 0 (`0x08000000 ~ 0x08003FFF`, 16KB)，移除所有 W25QXX SPI Flash 依赖。
     - 启动逻辑：
       - 读取 `RTC_BKP0R`，并立即将 `RTC_BKP0R` 清零；
       - 判断是否满足进入 Bootloader 驻留模式的任一条件：
         a) `get_boot_flag_magic(bkp) == BOOT_FLAG_MAGIC && get_boot_flag(bkp) == BOOT_BOOTLOADER` (`0x20300001`)；
         b) 物理恢复按键（`SPACE + F` 或 `SPACE + J`）按下；
         c) 内部 Flash `0x08004000` 处的 Kernel 向量表不合法 (`!is_internal_kernel_valid()`)。
       - 若不满足上述条件，则立即 `jump_to_kernel(0x08004000)` 启动 NOS。
       - 若满足条件，则点亮蓝色 RGB 状态灯，初始化 USB OTG FS WinUSB 设备（VID `0x0EFF`, PID `0x0001`），运行极简 NLK 协议循环（支持 `NLK_CAP_CORE` 与 `NLK_CAP_FLASH`）：
         - 注册分区 `"bootloader"`（只读描述）与 `"kernel"`（`max_size = 240KB`, `flags = WRITABLE | REBOOT_AFTER`，**不带** `NEEDS_BL`）；
         - 收到 `"kernel"` 的 `PREPARE` 时：校验 `total_size <= 240KB`，按 `total_size` 擦除 STM32 内部 Flash 对应的 Sector 1~5 (`0x08004000+`)；
         - 收到 `"kernel"` 的 `WRITE` 时：在 `offset == 0` 时校验向量表并暂存首字 `first_word`（`0x08004000` 处的 MSP），其余字直接按字编程写入 `0x08004000 + offset`（掉电安全：在全文校验通过前 `0x08004000` 保持 `0xFFFFFFFF`）；
         - 收到 `"kernel"` 的 `COMMIT` 时：刷新 ART 缓存，计算 `first_word` + `0x08004004..end` 的 CRC32 与 `expected_crc32` 比对；通过后将 `first_word` 写入 `0x08004000`，回复 NLK `COMMIT` 响应包。
4. **主机端 `nlk flash` 流程（`nlk_cli`）**：
   - `nlk flash --list`：调用 `NLK_FLASH_OP_QUERY_PARTS` 打印设备当前模式与所有注册分区表。
   - `nlk flash <partition> <file>`：
     1. 读取本地文件 `<file>`，计算全文件 `total_size` 与 `expected_crc32`；
     2. 调用 `NLK_FLASH_OP_QUERY_PARTS` 查找匹配的 `<partition>`（支持按名称如 `kernel`/`bootloader`/`assets` 或数字索引匹配）；若不存在则报错列出可用分区；
     3. 检查 `total_size <= part.max_size`；
     4. 若 `part.flags & NLK_FLASH_PART_F_NEEDS_BL` 且当前 `run_mode != BOOT_BOOTLOADER`：
        - 打印提示 `"Partition 'kernel' requires bootloader mode, rebooting device into bootloader..."`；
        - 发送 `NLK_CORE_OP_REBOOT(BOOT_BOOTLOADER)`，关闭当前连接，轮询等待目标设备在 Bootloader 模式下重连（超时 5 秒），重连后再次查询确认 `"kernel"` 分区就绪；
     5. 执行 `NLK_FLASH_OP_PREPARE` -> 循环分块 `NLK_FLASH_OP_WRITE`（实时显示进度百分比与速率） -> `NLK_FLASH_OP_COMMIT`；
     6. 若 `part.flags & NLK_FLASH_PART_F_REBOOT_AFTER`，自动发送 `NLK_CORE_OP_REBOOT(BOOT_NORMAL)` 重启进入新固件。

---

### 3.5 `NLK_CAP_VFS` 远端文件系统协议与 `nlk push` / `nlk pull` / `nlk ls`

在 `app/server/nlk/caps/cap_vfs.c` 实现完整的 `NLK_CAP_VFS (0x0004U)` 能力，对接 `include/fs/vfs.h`（仅在 `CONFIG_VFS=y` 时编译并注册到 `nlk_server`）：

1. **UAPI 操作码与结构体（`include/uapi/nos/nlk.h`）**：
   - 操作码：
     - `NLK_VFS_OP_OPEN (0x01)`：打开文件（转换跨平台标志：`NLK_VFS_O_RDONLY=1`, `NLK_VFS_O_WRONLY=2`, `NLK_VFS_O_RDWR=3`, `NLK_VFS_O_CREAT=0x0100`, `NLK_VFS_O_TRUNC=0x0200`, `NLK_VFS_O_APPEND=0x0400`），返回会话槽位 `handle` 与当前文件大小 `size`。
     - `NLK_VFS_OP_CLOSE (0x02)`：关闭 `handle`。
     - `NLK_VFS_OP_READ (0x03)`：给定 `{ handle, offset, count }`，调用 `vfs_lseek` + `vfs_read` 返回文件数据块。
     - `NLK_VFS_OP_WRITE (0x04)`：给定 `{ handle, offset, data... }`，调用 `vfs_lseek` + `vfs_write` 写入数据块并返回实际写入字节数。
     - `NLK_VFS_OP_STAT (0x05)`：查询路径元数据 `{ type, mode, size(64-bit), mtime(64-bit) }`。
     - `NLK_VFS_OP_READDIR (0x06)`：无状态游标式目录读取 `{ start_index, path }`，设备端通过 `vfs_opendir`/`vfs_readdir`/`vfs_closedir` 打包多个 `struct nlk_vfs_dirent_wire` 条目返回 `{ next_index, eof, count, entries[] }`。
     - `NLK_VFS_OP_MKDIR (0x07)`：调用 `vfs_mkdir`。
     - `NLK_VFS_OP_REMOVE (0x08)`：根据目标是文件还是目录调用 `vfs_unlink` 或 `vfs_rmdir`。
     - `NLK_VFS_OP_RENAME (0x09)`：调用 `vfs_rename(oldpath, newpath)`。
     - `NLK_VFS_OP_STATFS (0x0A)`：调用 `vfs_statfs(path)` 返回 `{ bsize, blocks, bfree, bavail, namelen }`。
2. **能力支持校验**：
   - 在 `nlk_cli` 中，`nlk push`、`nlk pull`、`nlk ls` 执行前首先检查目标设备是否具备 `NLK_CAP_VFS` 能力；若设备未注册 `NLK_CAP_VFS`（例如处于 Bootloader 模式或无 VFS 的板级），立即报错退出（错误码非 0）。
3. **CLI 子命令实现**：
   - `nlk ls [remote_path]`：默认 `/`（或 `/assets`），通过 `NLK_VFS_OP_READDIR` 列出目录下所有文件/子目录的类型、大小与名称。
   - `nlk push <local_path> <remote_path>`：支持单文件分块上传以及本地目录递归上传（自动在远端 `MKDIR` 并逐文件分块写入）。
   - `nlk pull <remote_path> <local_path>`：支持远端单文件分块下载以及远端目录递归下载。

---

### 3.6 多设备枚举与 `nlk -s <device>` 选择器

1. **增强设备枚举标识（`nlk_cli`）**：
   - `DiscoveredDevice` 结构增加 `serial`（USB `iSerialNumber` 字符串或设备唯一标识）与拓扑位置 `location`（如 `usb:20-5`、`/dev/cu.usbmodem*`、`mock://default`）。
   - `LibUsbTransport` 打开设备时支持按选择器过滤（匹配 `serial`、`usb:<bus>-<addr>`、`usb://...` 或设备名）。
2. **全局 `-s <device>` 参数**：
   - 支持 `nlk -s <device> <subcmd> ...`。
   - 当未指定 `-s` 且未指定 `--uri` 时：
     - 若扫描到恰好 1 台真实硬件设备，自动连接该设备；
     - 若扫描到超过 1 台真实硬件设备，拒絕盲目连接第一台，打印在线设备列表并提示使用 `nlk -s <device>` 指定目标设备。

---

## 4. 测试与验证计划

1. **NOS 主机回归测试 (`tests/host/test_nlk.c`)**：
   - 覆盖新 `NLK_CAP_FLASH`（动态分区注册、`QUERY_PARTS`、`PREPARE`/`WRITE`/`COMMIT` 原始镜像 CRC32 校验、`NEEDS_BL` 拦截）、`NLK_CAP_VFS`（`OPEN`/`READ`/`WRITE`/`CLOSE`/`STAT`/`READDIR`/`MKDIR`/`REMOVE`/`RENAME`/`STATFS`）、`NLK_CORE_OP_REBOOT`（`BOOT_NORMAL`/`BOOT_BOOTLOADER`）、`NLK_CAP_LOG` 实时流以及 `NLK_CAP_SHELL` 统一 `nos_tty` 前端交互。
2. **NOS 四大板级构建与 Bootloader 构建验证**：
   - `nk60-v2`、`qemu-stm32`、`wl-001`、`zj-v3` 四大 Preset 零错误零警告构建通过；
   - `bootloader` (`cmake --preset nk60-v2 && cmake --build --preset nk60-v2` in `bootloader/`) 构建通过且体积严控在 16KB Sector 0 以内（目标 < 8KB）；
   - `python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf` 与 `python3 scripts/check_layer_deps.py` 通过。
3. **`nlk_cli` 构建与单元测试 (`tests/test_nlk_cli.cpp`)**：
   - 覆盖 `nlk shell`（交互/非交互）、`nlk log`、`nlk flash`（含 `--list` 及按分区名刷写）、`nlk reboot [bootloader]`、`nlk push`/`nlk pull`/`nlk ls`（含无 `NLK_CAP_VFS` 时的报错验证）及 `nlk -s <device>` 选择器测试。

