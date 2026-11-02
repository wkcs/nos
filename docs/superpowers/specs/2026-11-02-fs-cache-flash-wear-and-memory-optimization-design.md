# NK60-V2 内存回收、通用 Block Cache、LittleFS 引入与 Flash 擦写防抖设计规范

- **状态**：已评审通过 (Approved)
- **日期**：2026-11-02
- **架构目标**：ARM Cortex-M4 (当前 NK60-V2, 46KB RAM) 并在设计上扩展兼容未来多核大内存 (ARM64 / RISC-V / x86_64 SMP Linux-like)

---

## 1. 背景与核心问题阐述

在 NK60-V2 真机运行实测中，固件暴露出深层次的内存濒危、锁争用与 Flash 寿命问题：
1. **系统可用空闲堆仅剩 1KB**：`keyboard_server` (4KB)、`nlk_server` (4KB)、`nshell` (2KB)、`nshell_nlk` (2KB) 静态栈过度配置，导致全局堆长期濒临 OOM；
2. **Profile 保存报 -ENOMEM**：`keyboard_config_save_profile` / `load_profile` 使用 `kmalloc(6,250)` 分配超大静态结构体，在仅剩 1KB 堆的环境中必定失败，导致硬件组合快捷键 `FN + 1~4` 与配置保存功能瘫痪；
3. **1.6 秒锁阻塞引发丢键卡死**：上位机进行背光调节、按键修改等 RPC 时，在持有 `ks->lock` 的状态下同步执行 FATFS 写入（涉及 open/write/close/unlink/rename，耗时 1.55~1.65 秒），导致 1ms 周期的按键扫描循环被独占挂起 1.6 秒，引发严重的打字卡死与 HID 报文丢失；
4. **Flash 寿命急剧磨损**：FATFS 在裸 SPI NOR Flash 上频繁原地覆写 FAT 表和目录项扇区，底层 W25Q Wear-Leveling 驱动每次均触发 4KB 物理扇区擦除；缺乏写回防抖与写合并缓存，导致 Flash 寿命损耗过快。

---

## 2. 总体架构分层与双模演进

为了满足当前 Cortex-M4 极限资源（46KB RAM），同时为未来多核大内存架构奠定坚实基石，系统采取分层与双模（Dual-Profile）抽象架构：

```
+-----------------------------------------------------------------------------------+
|                        POSIX / UAPI 统一接口契约                                   |
|   nos_open() / nos_write() / nos_read() / nos_close() / nos_sync() / nos_fsync()    |
+-----------------------------------------+-----------------------------------------+
                                          |
+-----------------------------------------v-----------------------------------------+
|                                NOS VFS 抽象层                                     |
|   struct super_operations:  .sync_fs(sb, wait)                                    |
|   struct file_operations:   .fsync(file, start, end, datasync)                    |
+--------------------+-------------------------------------+------------------------+
                     |                                     |
        +------------v------------+           +------------v------------+
        |   FatFs (兼容 SD卡/U盘)  |           | LittleFS (SPI NOR 推荐) |
        +------------+------------+           +------------+------------+
                     |                                     |
+--------------------v-------------------------------------v------------------------+
|                          通用块缓存抽象层 (Block Cache)                           |
|                                                                                   |
|  [MCU 紧凑模式 - NK60-V2 当前]            [SMP 多核/大内存模式 - 未来 Linux 演进]    |
|  - 8 × 512B 预分配扇区池 (4KB 黄金尺寸)   - 哈希桶 + 细粒度分桶 Spinlock           |
|  - 纯 BSS/预分配，零动态内存碎片           - 并发多核无锁读取 (RCU/读写锁)          |
|  - 极简单互斥锁保护                       - 后台独立 Flush 工作线程                |
|  - 1:1 匹配 W25Q 4KB 物理擦除粒度         - 内存高低水位线脏页自动清洗             |
+-----------------------------------------+-----------------------------------------+
                                          |
+-----------------------------------------v-----------------------------------------+
|                          底层驱动层 (Block Drivers)                               |
|            w25q_wl / w25qxx SPI NOR Flash / SD Card / NVMe / Ramdisk              |
+-----------------------------------------------------------------------------------+
```

---

## 3. 四阶段实施路线

### 3.1 Phase 1：内存回收与任务栈优化（即刻解除 OOM 风险）

#### 3.1.1 Kconfig 增设 `CONFIG_SHELL_CONSOLE`
- 在 `app/shell/Kconfig` 中增设：
  ```kconfig
  config SHELL_CONSOLE
      bool "Enable UART console nshell instance"
      default y
      depends on SHELL
      help
        Start an interactive nshell instance on the default console TTY.
        If disabled, other nshell transports (like nshell_nlk over USB)
        can still function normally.
  ```
- 在 `app/shell/nshell.c` 中：
  ```c
  #ifdef CONFIG_SHELL_CONSOLE
  static int nshell_init(void)
  {
      return nshell_spawn(nos_console_tty(), "nshell", true);
  }
  task_init(nshell_init);
  #endif
  ```
- 在 `configs/nk60-v2_defconfig` 中配置：
  `CONFIG_SHELL_CONSOLE=n`
- **收益**：裁撤 UART console `nshell` 任务，回收 1 个任务控制块 + 2048 字节栈 + 约 1200 字节 session 实例，**净释放 ~3.5KB 堆内存**。USB NLK 通道上的 `nshell_nlk` 维持完整运行。

#### 3.1.2 核心任务栈按实测峰值精确裁剪
- `app/server/keyboard/keyboard_server.c`：
  - 栈分配从 4096 字节降为 **2048 字节**（实测峰值 1440 字节，保留 608 字节 / ~30% 充足安全裕量，净释放 **2KB** 堆内存）。
- `app/server/nlk/nlk_server.c`：
  - 栈分配从 4096 字节降为 **3072 字节**（实测峰值 2448 字节，保留 624 字节安全裕量，净释放 **1KB** 堆内存）。
- `configs/nk60-v2_defconfig`：
  - `CONFIG_SHELL_STACK_SIZE` 从 2048 调整为 **1536 字节**（`nshell_nlk` 实测峰值 1288 字节，保留 248 字节安全裕量，净释放 **512 字节** 堆内存）。

#### 3.1.3 Profile 存储 6.2KB 动态分配改造
- 在 `keyboard_config_save_profile` 与 `keyboard_config_load_profile` 中，彻底消除 `kmalloc(sizeof(struct keyboard_stored_profile))`。
- 采用专用静态缓冲区或流式分块读写，配合 Phase 1 回收扩容后的 8KB 堆，彻底消除 `-ENOMEM` 缺陷，恢复 `FN + 1~4` 快捷键。

---

### 3.2 Phase 2：通用 Block Cache 与 `nos_sync` / `nos_fsync` 系统调用

#### 3.2.1 POSIX / VFS 接口契约
- 在 `include/fs/fs.h` 中导出标准接口：
  ```c
  int nos_sync(void);
  int nos_fsync(int fd);
  int nos_fdatasync(int fd);
  ```
- 在 `include/fs/vfs.h` 中完善核心结构体：
  ```c
  struct file_operations {
      /* ... read, write, open, release ... */
      int (*fsync)(struct file *file, loff_t start, loff_t end, int datasync);
  };

  struct super_operations {
      /* ... */
      int (*sync_fs)(struct super_block *sb, int wait);
  };
  ```
- 在 `fs/vfs_io.c` 中实现：
  - `nos_fsync(fd)`：定位文件，优先触发 `file->f_op->fsync`，刷新文件相关的 Dirty 块；
  - `nos_sync()`：遍历所有已挂载文件系统的 `super_block` 触发 `sync_fs`，并统筹刷新底层 Block Cache 的 Dirty 槽位。

#### 3.2.2 通用 Block Cache 核心设计
- 抽象模块位于 `drivers/block/block_cache.c` 与 `include/drivers/block_cache.h`。
- **4KB 黄金尺寸（8 个 512B 扇区）**：
  - 通过 Kconfig 提供 `CONFIG_BLOCK_CACHE_SECTORS`（默认 8，占用 4KB RAM）。
  - 恰好覆盖并吸收 SPI Flash 的 1 个完整 4KB 物理擦除扇区内所有细粒度修改。
- **核心数据结构**：
  ```c
  struct block_cache_entry {
      uint32_t lba;                /* 逻辑块地址 */
      uint32_t flags;              /* VALID, DIRTY */
      uint32_t access_seq;         /* LRU 计数 */
      uint8_t *data;               /* 扇区缓冲区 (512B) */
  };

  struct block_cache {
      const struct device *raw_bdev;
      uint32_t sector_size;
      uint16_t entry_count;
      uint32_t seq_counter;
      struct mutex lock;
      struct block_cache_entry *entries;
      uint8_t *data_pool;
  };
  ```
- **读写逻辑**：
  - `block_cache_write`：命中即在 RAM 覆写并置位 `DIRTY`；未命中则按 LRU 置换，若淘汰块为 `DIRTY` 则写回底盘。
  - `block_cache_sync`：将所有 `DIRTY` 块按 LBA 顺序回写物理介质并清标志。
- **FatFs `diskio.c` 桥接**：
  - `disk_ioctl(..., CTRL_SYNC, ...)` 真实调用 `block_cache_sync`，消除原有空操作。

---

### 3.3 Phase 3：引入并适配 LittleFS（彻底解决 Flash 原地擦写）

#### 3.3.1 LittleFS 核心属性
- 源码存放在 `libs/third_party/littlefs/`（`lfs.c`, `lfs.h`, `lfs_util.h`，代码尺寸极小）。
- VFS 驱动位于 `fs/littlefs/littlefs_vfs.c`，注册为 `"littlefs"`。
- **核心参数配置**：
  - `read_size = 256`
  - `prog_size = 256`（完美匹配 W25Q Page 编程）
  - `block_size = 4096`（匹配 4KB 扇区擦除）
  - `cache_size = 256`（单页缓存）
  - `lookahead_size = 32`（仅 32 字节分配位图）
  - `block_cycles = 500`（磨损均衡阈值）

#### 3.3.2 运行机制与掉电安全
- 纯 Copy-on-Write（COW）与追加写入，永远写入新物理块后更新元数据指针，彻底告别原地擦写。
- 提供 `CONFIG_ASSETS_FS_LITTLEFS`（优先启用）与 `CONFIG_ASSETS_FS_FATFS`（兼容保留），首次挂载若无文件系统自动格式化。

---

### 3.4 Phase 4：键盘服务配置异步防抖与锁隔离（消除丢键与 1.6s 阻塞）

#### 3.4.1 锁粒度解耦（Lock Decoupling）
- `ks->lock` 仅保护 RAM 中的按键硬件状态、扫描矩阵与 USB HID 上报；
- 严禁在持有 `ks->lock` 期间调用任何文件读写或 Flash 擦写。

#### 3.4.2 写回防抖与异步落盘（Debounce & Snapshot）
- 上位机发送 RPC 或组合键切层时：
  1. 获取 `ks->lock`，更新 RAM 状态（微秒级）；
  2. 标记 `config_dirty = true`，刷新防抖截止期 `sync_deadline_ms = now + 1000ms`；
  3. 释放 `ks->lock`，立即返回 RPC ACK（时延 < 1ms）；
- 当连续改动停止达到 1000ms 时：
  1. 瞬间持锁获取 300 字节配置快照（Snapshot）并释放 `ks->lock`；
  2. 由工作任务在无锁状态下从容调用 `keyboard_config_active_save()` 并触发 `nos_sync()`；
  3. 核心按键扫描任务（`keyboard_server_entry`）全程 1ms 毫秒级轮询，彻底告别丢键。

---

## 4. 收益指标量化对比

| 核心指标 | 优化前状态 | 经过 4 个阶段重构后 |
| :--- | :--- | :--- |
| **系统可用空闲堆** | 仅剩 **1 KB**（濒临 OOM 崩溃） | 扩容至 **~8 KB**（7 倍可用内存空间） |
| **Profile 保存 / 快捷键** | `kmalloc(6.2KB)` 报 `-ENOMEM` 失败 | 彻底解除 OOM，快捷键与保存完全正常 |
| **上位机 RPC 交互延迟** | 同步写盘等待 **1.6 秒** | 内存秒级响应，**< 1 毫秒** 返回 ACK |
| **按键扫描实时性** | 调参时被挂起 1.6 秒（严重丢键） | **1ms 恒定扫描**，核心矩阵扫描零阻塞 |
| **Flash 擦除磨损** | 每次修改立即触发 4KB 原地物理擦除 | LittleFS COW + 防抖合并，**寿命提升数十倍** |

---

## 5. 验证与回归策略

1. **主机与跨板回归**：
   - 执行 `python3 tests/run_host_regressions.py`、`cmake -P tests/test_arch_flags.cmake`；
   - 确保 `qemu-stm32`、`zj-v3`、`wl-001`、`nk60-v2` 四个 preset 全部构建零警告通过。
2. **QEMU 冒烟验证**：
   - `python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf`。
3. **NK60-V2 真机联调**：
   - 固件尺寸检查：确保 text 段不超过 240KB 内部 Flash 限额；
   - 运行时内存检查：`nlk shell free` 观察系统空闲堆是否从 1KB 扩充至 4KB~8KB；
   - Profile 保存验证：执行 `nlk kb profile save 1` 与硬件按键 `FN + 1~4` 检查是否成功返回；
   - 连续调参打字测试：上位机高频拉动背光亮度滑块，同时连续键入字符，验证 USB HID 是否平稳上报、零丢键。
