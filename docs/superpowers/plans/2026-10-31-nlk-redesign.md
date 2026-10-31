# NOS 与 NLK 核心能力重构实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 重新设计并实现 NOS 与 `nlk_cli` 的核心能力：统一内核 `boot_flag` 与 `reboot` 命令、UART 与 NLK 共用 Shell 服务及 `nlk shell`（交互/非交互）、`nlk log` 实时日志流、`NLK_CAP_FLASH` 通用分区注册与 STM32 内部 Flash 直刷、`NLK_CAP_VFS` 远端文件系统协议（`nlk push`/`nlk pull`/`nlk ls`），以及 `nlk -s <device>` 多设备选择能力。

**Architecture:** 在 NOS 内核侧，通过 `include/kernel/boot.h` 统一启动模式标志，通过 `struct nos_tty` 统一 UART 与 NLK Shell 前端，通过 `kernel_log_backend` 实时分发内核日志；在 NLK 协议层（`include/uapi/nos/nlk.h`），采用 32/64 位固定位宽结构体定义 `NLK_CAP_FLASH`（板级与独立 Bootloader 注册分区并直刷 STM32 内部 Flash）与 `NLK_CAP_VFS`（支持分块读写与游标式目录枚举）；在主机侧 `/Volumes/nick-disk/projects/nk60/nlk_cli`，同步 UAPI 协议并实现完整 CLI 与单元测试。

**Tech Stack:** C99/GNU11 (NOS Kernel & Bare-metal STM32F4 Bootloader), C++20 (`nlk_cli`), CMake + Ninja, Python 3 (Host Regression Runner).

## Global Constraints

- 代码缩进统一使用 4 个空格，严禁使用 Tab；采用 K&R 大括号风格与 `snake_case` 命名。
- 代码注释与文档统一使用中文编写。
- UAPI 头文件 `include/uapi/nos/nlk.h` 与 `nlk_cli/include/nlk/uapi.hpp` 严禁使用 `long`、`size_t`、`off_t`、`void *` 等平台相关位宽类型，全部使用定宽小端整数并标注 `__attribute__((packed))`。
- `flash` 分区注册属于 NLK 协议能力 (`NLK_CAP_FLASH`)，不在 NOS 内核中新增 `flash_partition.h`。
- `boot_flag` 与 `reboot` 属于 NOS 内核能力（定义于 `include/kernel/boot.h`），nShell 与 NLK 均调用内核 `cpu_reboot()`。
- `nlk push`、`nlk pull`、`nlk ls` 属于 `NLK_CAP_VFS` 能力，当目标设备不支持 `NLK_CAP_VFS` 时必须报错返回非 0 退出码。
- 移除旧脚本 `scripts/nlk_tool.py`，所有主机端工具统一使用 `/Volumes/nick-disk/projects/nk60/nlk_cli`。

---

### Task 1: 内核 Boot Flag 重构与 NLK UAPI 协议头文件升级

**Files:**
- Modify: `/Volumes/nick-disk/projects/nos/include/kernel/boot.h`
- Modify: `/Volumes/nick-disk/projects/nos/include/kernel/cpu.h`
- Modify: `/Volumes/nick-disk/projects/nos/kernel/boot.c`
- Modify: `/Volumes/nick-disk/projects/nos/include/uapi/nos/nlk.h`
- Modify: `/Volumes/nick-disk/projects/nos/include/nos/nlk_core.h`
- Modify: `/Volumes/nick-disk/projects/nos/include/nos/nlk_caps.h`
- Modify: `/Volumes/nick-disk/projects/nos/app/server/nlk/core/nlk_discovery.c`
- Delete: `/Volumes/nick-disk/projects/nos/scripts/nlk_tool.py`

**Interfaces:**
- Produces:
  - `enum boot_flag { BOOT_NORMAL = 0, BOOT_BOOTLOADER = 1, BOOT_RECOVERY = 2 }` in `include/kernel/boot.h`
  - `write_boot_flag(u32 flag)` (自动附加 `BOOT_FLAG_MAGIC 0x20300000UL`) 与 `read_boot_flag(void)` (校验 Magic 并返回 `enum boot_flag`) in `kernel/boot.c`
  - `NLK_CAP_FLASH (0x0001U)`, `NLK_CAP_VFS (0x0004U)`, `NLK_CORE_OP_REBOOT (0x06U)` 及对应定宽 Wire 结构体 in `include/uapi/nos/nlk.h`
  - `nlk_cap_flash_*` 与 `nlk_cap_vfs_*` 声明 in `include/nos/nlk_caps.h`

- [ ] **Step 1: 统一 `include/kernel/boot.h`、`include/kernel/cpu.h` 与 `kernel/boot.c`**

修改 `/Volumes/nick-disk/projects/nos/include/kernel/boot.h`：
```c
#ifndef __NOS_BOOT_H__
#define __NOS_BOOT_H__

#include <kernel/kernel.h>

/* 高 16 位 Magic 用于区分冷上电复位值 (0x00000000) 与显式重启标志 */
#define BOOT_FLAG_MAGIC         0x20300000UL
#define BOOT_FLAG_MAGIC_MASK    0xFFFF0000UL
#define BOOT_FLAG_MASK          0x0000FFFFUL

#define get_boot_flag(data)       ((u32)(BOOT_FLAG_MASK & (u32)(data)))
#define get_boot_flag_magic(data) ((u32)(BOOT_FLAG_MAGIC_MASK & (u32)(data)))
#define get_boot_flag_data(flag)  ((u32)(((u32)(flag) & BOOT_FLAG_MASK) | BOOT_FLAG_MAGIC))

enum boot_flag {
    BOOT_NORMAL     = 0,
    BOOT_BOOTLOADER = 1,
    BOOT_RECOVERY   = 2,
};

int write_boot_flag(u32 flag);
u32 read_boot_flag(void);

#endif /* __NOS_BOOT_H__ */
```

修改 `/Volumes/nick-disk/projects/nos/include/kernel/cpu.h`，移除冲突的 `enum cpu_boot_flag`，改为引入 `<kernel/boot.h>`：
```c
#include <kernel/kernel.h>
#include <kernel/boot.h>
```

修改 `/Volumes/nick-disk/projects/nos/kernel/boot.c`：
```c
#include <kernel/boot.h>
#include <kernel/kernel.h>
#include <kernel/board.h>

int write_boot_flag(u32 flag)
{
    u32 raw = (get_boot_flag_magic(flag) == BOOT_FLAG_MAGIC) ?
              flag : get_boot_flag_data(flag);
    return board_write_boot_flag(raw);
}

u32 read_boot_flag(void)
{
    u32 raw = board_read_boot_flag();
    if (get_boot_flag_magic(raw) != BOOT_FLAG_MAGIC) {
        return BOOT_NORMAL;
    }
    return get_boot_flag(raw);
}
```

- [ ] **Step 2: 升级 `include/uapi/nos/nlk.h`、`include/nos/nlk_core.h` 与 `include/nos/nlk_caps.h`**

在 `/Volumes/nick-disk/projects/nos/include/uapi/nos/nlk.h` 中：
1. 将 `NLK_CAP_FW_UPDATE = 0x0001U` 替换为 `NLK_CAP_FLASH = 0x0001U`。
2. 在 `enum nlk_core_opcode` 中增加 `NLK_CORE_OP_REBOOT = 0x06U` 及请求结构体 `struct nlk_core_reboot_req { uint32_t boot_flag; } __attribute__((packed));`。
3. 定义 `NLK_CAP_FLASH` 协议结构体：
   - `enum nlk_flash_opcode`: `NLK_FLASH_OP_QUERY_PARTS = 0x01U`, `NLK_FLASH_OP_PREPARE = 0x02U`, `NLK_FLASH_OP_COMMIT = 0x03U`, `NLK_FLASH_OP_ABORT = 0x04U`, `NLK_FLASH_BULK_WRITE = 0x10U`
   - 分区标志：`NLK_FLASH_PART_F_WRITABLE (1U << 0)`, `NLK_FLASH_PART_F_NEEDS_BL (1U << 1)`, `NLK_FLASH_PART_F_REBOOT_AFTER (1U << 2)`
   - `struct nlk_flash_part_desc` (28 字节)、`struct nlk_flash_query_resp`、`struct nlk_flash_prepare_req` (16 字节)、`struct nlk_flash_commit_req` (12 字节)、`struct nlk_flash_chunk_hdr` (8 字节：`part_id`, `reserved[3]`, `offset`)、`struct nlk_flash_chunk_ack` (8 字节)。
4. 定义 `NLK_CAP_VFS` 协议结构体：
   - `enum nlk_vfs_opcode`: `NLK_VFS_OP_OPEN = 0x01U`, `NLK_VFS_OP_CLOSE = 0x02U`, `NLK_VFS_OP_READ = 0x03U`, `NLK_VFS_OP_WRITE = 0x04U`, `NLK_VFS_OP_STAT = 0x05U`, `NLK_VFS_OP_READDIR = 0x06U`, `NLK_VFS_OP_MKDIR = 0x07U`, `NLK_VFS_OP_REMOVE = 0x08U`, `NLK_VFS_OP_RENAME = 0x09U`, `NLK_VFS_OP_STATFS = 0x0AU`
   - 跨平台打开标志：`NLK_VFS_O_RDONLY`, `NLK_VFS_O_WRONLY`, `NLK_VFS_O_RDWR`, `NLK_VFS_O_CREAT`, `NLK_VFS_O_TRUNC`, `NLK_VFS_O_APPEND`
   - 目录项类型：`NLK_VFS_DT_UNKNOWN = 0U`, `NLK_VFS_DT_REG = 1U`, `NLK_VFS_DT_DIR = 2U`, `NLK_VFS_DT_CHR = 3U`, `NLK_VFS_DT_BLK = 4U`
   - 定宽请求与响应结构体：`nlk_vfs_open_req`, `nlk_vfs_open_resp`, `nlk_vfs_close_req`, `nlk_vfs_read_req`, `nlk_vfs_read_resp`, `nlk_vfs_write_req`, `nlk_vfs_write_resp`, `nlk_vfs_stat_req`, `nlk_vfs_stat_resp`, `nlk_vfs_readdir_req`, `nlk_vfs_dirent_wire`, `nlk_vfs_readdir_resp`, `nlk_vfs_path_req`, `nlk_vfs_rename_req`, `nlk_vfs_statfs_resp`。

- [ ] **Step 3: 在 `app/server/nlk/core/nlk_discovery.c` 中支持 `NLK_CORE_OP_REBOOT` 并移除 `scripts/nlk_tool.py`**

在 `nlk_node_set_reboot_handler()` / `nlk_discovery.c` 中处理 `NLK_CORE_OP_REBOOT`：校验载荷长度并触发节点注册的 `on_reboot(boot_flag, priv)` 回调（若在内核态则启动延迟重启定时器调用 `cpu_reboot(boot_flag)`）。删除 `/Volumes/nick-disk/projects/nos/scripts/nlk_tool.py`。

---

### Task 2: `NLK_CAP_FLASH` 通用分区刷写能力与 `nk60-v2` / `bootloader` 直刷 STM32 内部 Flash

**Files:**
- Create: `/Volumes/nick-disk/projects/nos/app/server/nlk/caps/cap_flash.c`
- Delete: `/Volumes/nick-disk/projects/nos/app/server/nlk/caps/cap_fw_update.c`
- Modify: `/Volumes/nick-disk/projects/nos/app/server/sources.cmake`
- Modify: `/Volumes/nick-disk/projects/nos/app/server/nlk/core/nlk_router.c`
- Modify: `/Volumes/nick-disk/projects/nos/app/server/nlk/core/nlk_core.c`
- Modify: `/Volumes/nick-disk/projects/nos/board/nk60-v2/include/board/boot.h`
- Modify: `/Volumes/nick-disk/projects/nos/board/nk60-v2/nk60_nlk.c`
- Modify: `/Volumes/nick-disk/projects/nos/bootloader/include/boot_proto.h`
- Modify: `/Volumes/nick-disk/projects/nos/bootloader/main.c`
- Modify: `/Volumes/nick-disk/projects/nos/bootloader/CMakeLists.txt`

**Interfaces:**
- Consumes: `NLK_CAP_FLASH` UAPI 结构与 `BOOT_NORMAL`/`BOOT_BOOTLOADER`
- Produces:
  - `nlk_cap_flash_init(struct nlk_node *node, struct nlk_flash_ctx *ctx, uint8_t run_mode)`
  - `nlk_cap_flash_register_part(struct nlk_flash_ctx *ctx, const struct nlk_flash_part_ops *ops)`
  - `nk60-v2` 板级注册 `"bootloader"`, `"kernel"`, `"assets"` 分区
  - `bootloader/main.c` 内置裸机 USB OTG FS WinUSB + UART1 NLK Server，支持 `nlk reboot` 与直刷 `"kernel"` 内部 Flash Sector 1~5

- [ ] **Step 1: 实现 `app/server/nlk/caps/cap_flash.c` 并更新 `nlk_router.c` / `nlk_core.c` / `sources.cmake`**

实现 `cap_flash.c`：
- 管理最多 `NLK_FLASH_MAX_PARTS (8)` 个动态注册的分区 `struct nlk_flash_part_ops`；
- 处理 `NLK_FLASH_OP_QUERY_PARTS`：返回所有已注册分区的 `nlk_flash_part_desc`（含名称、容量、擦除块大小、标志位）以及当前 `run_mode`；
- 处理 `NLK_FLASH_OP_PREPARE`：校验 `part_id`、检查是否只读或带有 `NLK_FLASH_PART_F_NEEDS_BL`（若带 `NEEDS_BL` 则返回 `-EBUSY` 提示切入 Bootloader）、校验 `total_size <= max_size`，调用分区的 `prepare` 回调，初始化增量 CRC32 状态；
- 处理 `NLK_MODE_BULK` (`NLK_FLASH_BULK_WRITE`)：校验 `part_id` 与连续 `offset`，调用分区的 `write` 回调并增量更新 `running_crc32`，返回 `nlk_flash_chunk_ack`；
- 处理 `NLK_FLASH_OP_COMMIT`：校验 `received_bytes == total_size` 且 `running_crc32 == expected_crc32`，调用分区的 `commit` 回调完成硬件读回校验与落盘。

- [ ] **Step 2: 重构 `board/nk60-v2/nk60_nlk.c` 直接刷写 Sector 0 Bootloader 与 W25QXX Assets**

在 `board/nk60-v2/nk60_nlk.c` 中：
- 移除旧的 `nlk_fw_img_hdr` 头部剥离逻辑与 W25QXX Bootloader/Kernel 中转逻辑；
- 注册 3 个分区到 `brd->flash_cap`：
  1. `"bootloader"` (`16KB`, `erase_size = 16KB`, `flags = NLK_FLASH_PART_F_WRITABLE`)：
     - `prepare`：擦除 STM32F401 内部 Flash Sector 0 (`0x08000000`)；
     - `write`：`offset == 0` 时校验 Cortex-M4 向量表合法性，直接按 32-bit 字写入 `0x08000000 + offset`；
     - `commit`：刷新 ART Cache，校验 `0x08000000` 处 `total_size` 的 CRC32 与 `expected_crc32` 一致。
  2. `"kernel"` (`240KB`, `erase_size = 16KB`, `flags = NLK_FLASH_PART_F_WRITABLE | NLK_FLASH_PART_F_NEEDS_BL | NLK_FLASH_PART_F_REBOOT_AFTER`)：
     - 在 NOS 运行态下标记 `NEEDS_BL`，由 `nlk_cli` 自动触发 `reboot bootloader` 进入 Bootloader 直刷内部 Flash Sector 1~5。
  3. `"assets"` (`736KB`, `erase_size = 4KB`, `flags = NLK_FLASH_PART_F_WRITABLE`)：
     - 按扇区预擦除并写入外部 W25QXX `0x048000`，`commit` 时读回校验 CRC32 并调用 `nk60_assets_reload()`。
- 注册节点 `reboot` 回调：收到 `NLK_CORE_OP_REBOOT` 后启动 50ms 定时器执行 `cpu_reboot(boot_flag)`。

- [ ] **Step 3: 升级 `bootloader/main.c` 实现不经 SPI Flash 直刷 STM32 内部 Flash Sector 1~5**

在 `bootloader/include/boot_proto.h` 与 `bootloader/main.c` 中：
- 移除 W25QXX SPI1 驱动代码以及 `bootloader/CMakeLists.txt` 中对 `create_img.py` 的调用（保留生成 `bootloader.bin` 与 `bootloader.hex`）；
- 启动时读取 `RTC_BKP0R` 并立即清零 `RTC_BKP0R = 0UL`；
- 若 `(bkp == get_boot_flag_data(BOOT_BOOTLOADER))` (`0x20300001UL`)，或物理恢复组合键按下，或 `!is_internal_kernel_valid()`，则进入 Bootloader NLK 模式：
  - 初始化 STM32F401 USB OTG FS 设备控制器（PA11/PA12 AF10，VID `0x0EFF`, PID `0x0001`，EP0 Control + EP1 Bulk OUT `0x01` + EP1 Bulk IN `0x81`，兼容 WinUSB OS 描述符）以及 UART1 (`115200 8N1` L2 Stream 帧)；
  - 响应 `NLK_CAP_CORE`（`HELLO`/`GET_NODE_INFO`/`GET_CAPS`/`PING`/`REBOOT`）与 `NLK_CAP_FLASH`（暴露 `"bootloader"` 只读分区和 `"kernel"` 可写分区 `240KB`）；
  - 收到 `"kernel"` 的 `PREPARE` 时：按 `total_size` 擦除 STM32 内部 Flash 对应的 Sector 1~5 (`0x08004000+`)；
  - 收到 `"kernel"` 的 `BULK_WRITE` 时：首字 `first_word`（偏移 0 的 MSP）暂存于 RAM，其余字直接写入 `0x08004000 + offset`；
  - 收到 `"kernel"` 的 `COMMIT` 时：刷新 ART Cache，校验 `first_word` + `0x08004004..end` 的 CRC32 == `expected_crc32`，随后将 `first_word` 写入 `0x08004000` 并回复 ACK；
  - 收到 `NLK_CORE_OP_REBOOT` 时：回复 ACK，写入 `RTC_BKP0R` 并执行 `SCB_AIRCR = 0x05FA0004UL` 系统复位。
- 运行 `cmake --preset nk60-v2 && cmake --build --preset nk60-v2`（在 `bootloader/` 目录下）验证 `bootloader.bin` 编译通过且体积 < 16KB。

---

### Task 3: `NLK_CAP_VFS` 远端文件系统协议实现

**Files:**
- Create: `/Volumes/nick-disk/projects/nos/app/server/nlk/caps/cap_vfs.c`
- Modify: `/Volumes/nick-disk/projects/nos/app/server/sources.cmake`
- Modify: `/Volumes/nick-disk/projects/nos/app/server/nlk/nlk_server.c`

**Interfaces:**
- Consumes: `include/fs/vfs.h` (`vfs_open`, `vfs_close`, `vfs_read`, `vfs_write`, `vfs_lseek`, `vfs_stat`, `vfs_opendir`, `vfs_readdir`, `vfs_closedir`, `vfs_mkdir`, `vfs_unlink`, `vfs_rmdir`, `vfs_rename`, `vfs_statfs`)
- Produces: `nlk_cap_vfs_init(struct nlk_node *node, struct nlk_vfs_ctx *ctx)`

- [ ] **Step 1: 实现 `app/server/nlk/caps/cap_vfs.c`**

实现完整的 `NLK_CAP_VFS`：
- 维护固定大小的会话文件句柄池 `int open_fds[NLK_VFS_MAX_HANDLES]`（初始化为 `-1`）；
- `NLK_VFS_OP_OPEN`：将 `NLK_VFS_O_*` 标志转换为内核 `O_RDONLY`/`O_WRONLY`/`O_RDWR`/`O_CREAT`/`O_TRUNC`/`O_APPEND`，调用 `vfs_open()` 并通过 `vfs_fstat()` 获取文件大小，返回 `{ handle, size }`；
- `NLK_VFS_OP_CLOSE`：调用 `vfs_close()` 释放句柄槽位；
- `NLK_VFS_OP_READ`：调用 `vfs_lseek(fd, req->offset, SEEK_SET)` 与 `vfs_read(fd, buf, count)`，返回读取的数据块与 `eof` 标志；
- `NLK_VFS_OP_WRITE`：调用 `vfs_lseek(fd, req->offset, SEEK_SET)` 与 `vfs_write(fd, data, len)`，返回写入字节数 `written`；
- `NLK_VFS_OP_STAT`：调用 `vfs_stat(path, &st)`，将 `S_ISDIR`/`S_ISREG`/`S_ISCHR`/`S_ISBLK` 映射为 `NLK_VFS_DT_*`，返回 `{ type, mode, size, mtime }`；
- `NLK_VFS_OP_READDIR`：调用 `vfs_opendir(path)`，跳过前 `req->start_index` 个条目后，循环调用 `vfs_readdir()` 将条目打包为 `struct nlk_vfs_dirent_wire` 填入响应缓冲区直到接近 MTU 上限或读完，随后立即 `vfs_closedir()`，返回 `{ next_index, eof, count, entries[] }`；
- `NLK_VFS_OP_MKDIR`：调用 `vfs_mkdir(path, mode)`；
- `NLK_VFS_OP_REMOVE`：先 `vfs_stat(path, &st)`，若为目录则调用 `vfs_rmdir(path)`，否则调用 `vfs_unlink(path)`；
- `NLK_VFS_OP_RENAME`：调用 `vfs_rename(oldpath, newpath)`；
- `NLK_VFS_OP_STATFS`：调用 `vfs_statfs(path, &sfs)` 返回 `{ bsize, blocks, bfree, bavail, namelen }`。

- [ ] **Step 2: 在 `nlk_server.c` 与 `app/server/sources.cmake` 中集成 `cap_vfs.c`**

当 `CONFIG_VFS` 启用时，在 `nlk_server.c` 中调用 `nlk_cap_vfs_init(&g_nlk_root_node, &g_nlk_vfs_ctx)` 自动注册 `NLK_CAP_VFS`。

---

### Task 4: UART 与 NLK 共用 Shell 服务架构 + 内置 `reboot` 命令 + `nlk log` 实时日志解耦

**Files:**
- Modify: `/Volumes/nick-disk/projects/nos/include/nos/tty.h`
- Modify: `/Volumes/nick-disk/projects/nos/kernel/tty/tty.c`
- Modify: `/Volumes/nick-disk/projects/nos/app/server/nlk/caps/cap_shell.c`
- Modify: `/Volumes/nick-disk/projects/nos/app/shell/nshell_internal.h`
- Modify: `/Volumes/nick-disk/projects/nos/app/shell/nshell.c`
- Modify: `/Volumes/nick-disk/projects/nos/kernel/printk/printk.c`
- Modify: `/Volumes/nick-disk/projects/nos/app/server/nlk/caps/cap_log.c`

**Interfaces:**
- Consumes: `struct nos_tty`, `cpu_reboot(u32 flag)`, `log_publish()`
- Produces:
  - `struct nos_tty_ops` 新增 `poll_session(struct nos_tty *tty, bool *reconnected)` 与 `on_exit(struct nos_tty *tty, int32_t exit_code)`
  - `nshell_start_tty(const char *task_name, struct nos_tty *tty)` 统一启动任意 TTY 前端
  - nShell 内置 `reboot [normal|bootloader|recovery]` 命令

- [ ] **Step 1: 扩展 `include/nos/tty.h`、`kernel/tty/tty.c` 与 `app/server/nlk/caps/cap_shell.c`**

1. 在 `include/nos/tty.h` 的 `struct nos_tty_ops` 中添加可选回调：
   ```c
   bool (*poll_session)(struct nos_tty *tty, bool *reconnected);
   void (*on_exit)(struct nos_tty *tty, int32_t exit_code);
   ```
   并提供辅助内联函数 `nos_tty_poll_session(tty, &reconnected)` 和 `nos_tty_on_exit(tty, code)`。
2. 在 `kernel/tty/tty.c` 中，让 `console_tty_read()` 在无预注入输入时调用 `console_getc(&ch)`，使 `nos_console_tty()` 成为完整的阻塞/轮询可读可写 TTY 前端。
3. 在 `app/server/nlk/caps/cap_shell.c` 中，为 `g_nlk_shell_tty_ops` 实现 `poll_session`（检查 `session_open` 与 `session_gen` 变化）和 `on_exit`（调用 `nlk_cap_shell_exit_session(ctx, exit_code)`）。

- [ ] **Step 2: 重构 `app/shell/nshell.c` 消除 `shell->remote` 并新增 `reboot` 命令**

1. 从 `struct nshell_session` (`app/shell/nshell_internal.h`) 中移除 `bool remote` 字段。
2. 在 `app/shell/nshell.c` 中删除所有 `if (shell->remote)` 分支，统一通过 `shell->io_session.tty` 调用 `nos_tty_read()`、`nos_tty_write()`、`nos_tty_poll_session()` 和 `nos_tty_on_exit()`。
3. 在 `builtin_cmds[]` 中新增 `reboot` 命令（`cmd_reboot`）：
   - `reboot` 或 `reboot normal` -> `cpu_reboot(BOOT_NORMAL)`
   - `reboot bootloader` -> `cpu_reboot(BOOT_BOOTLOADER)`
   - `reboot recovery` -> `cpu_reboot(BOOT_RECOVERY)`
   - 非法参数打印用法并返回 `-EINVAL`。

- [ ] **Step 3: 解耦 `kernel/printk/printk.c` 的串口输出阈值与 `log_publish()` 后端分发**

在 `kernel/printk/printk.c` 的 `pr_log()` 中：
- 只要 `g_log_enabled` 为真，格式化日志后始终调用 `log_publish(level, log_buf, (u16)len)` 分发给所有已注册的日志后端（如 `cap_log`），而仅当 `level <= g_log_level` 时才写入本地串口控制台。这样即使本地串口只打印高优日志，`nlk log` 依然能按自身设置的 `min_level` 实时接收全部内核日志。

---

### Task 5: NOS 主机回归测试更新与四大板级编译验证

**Files:**
- Modify: `/Volumes/nick-disk/projects/nos/tests/host/run_nlk_test.py`
- Modify: `/Volumes/nick-disk/projects/nos/tests/host/test_nlk.c`
- Modify: `/Volumes/nick-disk/projects/nos/tests/stubs/fs/vfs.h` (或测试桩)

**Interfaces:**
- Consumes: Tasks 1-4 实现的 `cap_flash.c`, `cap_vfs.c`, `cap_shell.c`, `cap_log.c`, `nlk_discovery.c`

- [ ] **Step 1: 更新 `tests/host/run_nlk_test.py` 与 `tests/host/test_nlk.c`**

1. 在 `tests/host/run_nlk_test.py` 中将 `caps/cap_fw_update.c` 替换为 `caps/cap_flash.c` 和 `caps/cap_vfs.c`。
2. 在 `tests/host/test_nlk.c` 中编写完整单元测试：
   - 测试 `NLK_CAP_FLASH`：动态注册 `"bootloader"`、`"kernel"` (`NEEDS_BL`)、`"assets"` 分区，验证 `QUERY_PARTS` 返回正确的分区名与标志；验证对 `"kernel"` (`NEEDS_BL`) 发起 `PREPARE` 返回 `-EBUSY`；验证对 `"bootloader"` 发起原始二进制流 `PREPARE -> BULK_WRITE -> COMMIT` 及 CRC32 校验；
   - 测试 `NLK_CORE_OP_REBOOT`：验证传递 `BOOT_NORMAL` 与 `BOOT_BOOTLOADER` 触发节点 `on_reboot` 回调；
   - 测试 `NLK_CAP_VFS`：验证 `OPEN`、`WRITE`、`READ`、`CLOSE`、`STAT`、`READDIR`（含多条目打包与游标分页）、`MKDIR`、`REMOVE`、`RENAME`、`STATFS`；验证未注册 `NLK_CAP_VFS` 的节点（如 `node_knob`）访问 VFS 返回 `-ENOENT`。

- [ ] **Step 2: 运行 NOS 全量主机回归与四大板级构建验证**

Run:
```bash
python3 tests/run_host_regressions.py
python3 -m unittest discover -s tests -p 'test_*.py'
python3 scripts/check_layer_deps.py
cmake --preset nk60-v2 && cmake --build --preset nk60-v2
cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32
cmake --preset wl-001 && cmake --build --preset wl-001
cmake --preset zj-v3 && cmake --build --preset zj-v3
python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf
```
Expected: 全部测试与构建 100% 通过，零错误、零新增警告。

---

### Task 6: 主机端 `nlk_cli` 全面升级与单元测试验证

**Files:**
- Modify: `/Volumes/nick-disk/projects/nk60/nlk_cli/include/nlk/uapi.hpp`
- Modify: `/Volumes/nick-disk/projects/nk60/nlk_cli/include/nlk/transport.hpp`
- Modify: `/Volumes/nick-disk/projects/nk60/nlk_cli/include/nlk/client.hpp`
- Modify: `/Volumes/nick-disk/projects/nk60/nlk_cli/src/transport_libusb.cpp`
- Modify: `/Volumes/nick-disk/projects/nk60/nlk_cli/src/transport_serial.cpp`
- Modify: `/Volumes/nick-disk/projects/nk60/nlk_cli/src/transport_mock.cpp`
- Modify: `/Volumes/nick-disk/projects/nk60/nlk_cli/src/client.cpp`
- Modify: `/Volumes/nick-disk/projects/nk60/nlk_cli/src/bridge_server.cpp`
- Modify: `/Volumes/nick-disk/projects/nk60/nlk_cli/src/main.cpp`
- Modify: `/Volumes/nick-disk/projects/nk60/nlk_cli/tests/test_nlk_cli.cpp`

**Interfaces:**
- Consumes: `include/uapi/nos/nlk.h` 协议定义
- Produces:
  - `nlk [-s <device>] shell [cmd...]`（交互式 Raw 终端 + 非交互式命令执行）
  - `nlk [-s <device>] log [-l <level>] [-d <sec>]`（实时持续打印日志流，Ctrl+C 优雅退订）
  - `nlk [-s <device>] flash --list` 与 `nlk [-s <device>] flash <partition> <file>`（动态查询分区名、自动切换 Bootloader 并直刷原始镜像）
  - `nlk [-s <device>] reboot [normal|bootloader|recovery]`
  - `nlk [-s <device>] push <local> <remote>`、`nlk [-s <device>] pull <remote> <local>`、`nlk [-s <device>] ls [remote_path]`（强制校验 `NLK_CAP_VFS` 能力，不支持时报错）

- [ ] **Step 1: 同步 `nlk_cli/include/nlk/uapi.hpp` 与传输层 `transport_*.cpp`（支持 `-s <device>`）**

1. 将 `nlk_cli/include/nlk/uapi.hpp` 与 NOS 的 `include/uapi/nos/nlk.h` 完全对齐（使用 12 字节 `nlk_pkt_hdr`、CRC8/CRC16/CRC32、L2 Stream 帧与 Datagram 帧，以及 `NLK_CAP_CORE`、`NLK_CAP_FLASH`、`NLK_CAP_LOG`、`NLK_CAP_SHELL`、`NLK_CAP_VFS` 结构体）。
2. 在 `transport.hpp` 的 `DiscoveredDevice` 中增加 `serial` 与 `location` 字段；在 `transport_libusb.cpp` 中读取 USB `iSerialNumber` 与 `usb:<bus>-<addr>`，支持通过 `resolve_device_uri(selector)` 按序列号、拓扑位置、设备名称或 URI 精确匹配目标设备；当存在多台设备且未指定 `-s` 时报错提示。
3. 更新 `transport_mock.cpp`：模拟完整的 NOS 节点（含可配置的 `caps_bitmap`、动态分区表 `"bootloader"`/`"kernel"`/`"assets"`、内存 VFS 文件与目录树、实时日志流及交互式/非交互式 Shell）。

- [ ] **Step 2: 升级 `nlk_cli/src/client.cpp` 与 `nlk_cli/src/main.cpp`**

1. 在 `Client` 中实现：
   - 能力实例自动解析与 `require_capability(addr, cap_type)` 强校验（若设备 `caps_bitmap` 不含该能力则抛出包含能力名的明确错误）；
   - `reboot(addr, boot_flag)`：发送 `NLK_CORE_OP_REBOOT`；
   - `query_flash_parts(addr)` 与 `flash_partition(addr, part_name_or_id, image, progress_cb)`：动态匹配分区名，若带 `NLK_FLASH_PART_F_NEEDS_BL` 且当前处于 `BOOT_NORMAL`，自动发送 `reboot(BOOT_BOOTLOADER)` 并等待重连后完成 `PREPARE -> BULK_WRITE -> COMMIT`；
   - `shell_exec(addr, cmd)` 与 `shell_interactive(addr)`（跨平台 Raw 终端交互模式）；
   - `stream_logs(addr, min_level, duration_sec, on_record_cb, stop_flag)`：实时流式打印日志并在停止时退订；
   - `vfs_listdir(addr, path)`、`vfs_stat(addr, path)`、`vfs_statfs(addr, path)`、`vfs_mkdir(addr, path)`、`vfs_remove(addr, path)`、`vfs_rename(addr, oldp, newp)`、`vfs_push(addr, local_path, remote_path)`（支持文件与目录递归）、`vfs_pull(addr, remote_path, local_path)`（支持文件与目录递归）。
2. 在 `main.cpp` 中解析全局 `-s <device>` 参数及子命令：`list`、`discover`、`ping`、`reboot`、`shell`、`log`、`flash`、`push`、`pull`、`ls`、`bridge`（移除旧的 `exec` 和 `fw-update`）。

- [ ] **Step 3: 编写并运行 `nlk_cli/tests/test_nlk_cli.cpp` 单元测试**

Run:
```bash
cmake -S /Volumes/nick-disk/projects/nk60/nlk_cli -B /Volumes/nick-disk/projects/nk60/nlk_cli/build
cmake --build /Volumes/nick-disk/projects/nk60/nlk_cli/build
ctest --test-dir /Volumes/nick-disk/projects/nk60/nlk_cli/build --output-on-failure
```
验证覆盖：
1. `nlk shell` 非交互与交互会话流；
2. `nlk log` 实时日志流接收与退订；
3. `nlk flash --list` 与 `nlk flash bootloader/kernel/assets` 动态分区匹配与 CRC32 校验；
4. `nlk reboot` 与 `nlk reboot bootloader` 标志传递；
5. `nlk push`、`nlk pull`、`nlk ls` 文件分块读写与目录枚举，以及当节点不支持 `NLK_CAP_VFS` 时严格报错拒绝；
6. `nlk -s <device>` 在多设备场景下的精确匹配与未指定时的歧义拦截。

