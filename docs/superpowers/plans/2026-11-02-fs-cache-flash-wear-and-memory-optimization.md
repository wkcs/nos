# NK60-V2 内存回收、通用 Block Cache、LittleFS 引入与 Flash 擦写防抖实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 彻底解决 NK60-V2 (STM32F401, Cortex-M4) 固件中 1KB 堆内存极限濒危、Profile 保存报 -ENOMEM、1.6 秒持锁写 Flash 导致键盘扫描丢键卡死，以及 SPI NOR Flash 频繁原地擦除磨损等核心缺陷，并在架构上对齐 Linux VFS 契约、提供通用双模 Block Cache 与 `nos_sync/fsync`，引入 LittleFS 作为原生 Flash 文件系统。

**Architecture:** 
1. **内存与配置优化层**：通过 `CONFIG_SHELL_CONSOLE=n` 裁撤 UART 冗余 Shell（保留 USB NLK Shell），精简任务栈分配，重构 Profile 编解码为免堆内存流式读写，系统可用堆从 1KB 扩容至 ~8KB；
2. **通用 Block Cache 与 VFS 契约层**：在 VFS 实现标准 POSIX `nos_sync()` 与 `nos_fsync()` 接口，在底层引入通用 Block Cache 模块（NK60-V2 默认 8 扇区 / 4KB 黄金尺寸，1:1 覆盖 W25Q 4KB 物理擦除扇区），吸收 FATFS 高频写并支持未来多核 SMP 大内存扩展；
3. **专用 Flash 文件系统层**：移植轻量级 LittleFS 核心，实现 `littlefs_vfs.c` 桥接，支持 COW 追加写入与掉电安全，根治 NOR Flash 原地擦写；
4. **键盘应用层防抖与锁解耦**：将 `ks->lock` 严格限制为保护按键矩阵扫描状态，配置变更微秒级更新 RAM 并返回 ACK，通过 1000ms 防抖定时器与后台快照安全落盘，按键扫描保持 1ms 绝对流畅。

**Tech Stack:** C99 / GNU11, ARM Cortex-M4, CMake + Ninja + Kconfiglib, NOS Microkernel VFS, LittleFS v2, SPI NOR Flash (W25QXX / wear-leveling).

## Global Constraints

- **缩进风格**：严格 4 个空格缩进，禁止使用 Tab。
- **行尾序列**：Unix 风格 LF (`\n`)。
- **大括号风格**：Linux 内核 K&R 风格。
- **命名规范**：`snake_case` 用于函数、变量与文件名，`UPPER_SNAKE_CASE` 用于宏定义。
- **头文件隔离**：严格禁止在 `include/` 根目录创建 C 库同名头文件。
- **跨平台多板级兼容**：修改必须保证 `nk60-v2`、`qemu-stm32`、`zj-v3`、`wl-001` 四个 preset 全部构建零错误、零新增警告，且 QEMU 冒烟测试全绿。

---

### Task 1: 内存回收与任务栈精细化裁剪 (Phase 1)

**Files:**
- Modify: `Kconfig:377-405` (增加 `CONFIG_SHELL_CONSOLE`)
- Modify: `app/shell/nshell.c:6675-6681` (条件编译 console `nshell_init`)
- Modify: `configs/nk60-v2_defconfig:27-30` (配置 `CONFIG_SHELL_CONSOLE=n`, `CONFIG_SHELL_STACK_SIZE=1536`)
- Modify: `app/server/keyboard/keyboard_server.c:1175-1185` (任务栈从 4096 调整至 2048)
- Modify: `app/server/nlk/nlk_server.c:135-145` (任务栈从 4096 调整至 3072)

**Interfaces:**
- Produces: `CONFIG_SHELL_CONSOLE` 宏，当为 `n` 时不创建 console shell 任务。
- Consumes: `task_create()`，使用缩减后的安全栈尺寸。

- [ ] **Step 1: 在顶层 `Kconfig` 的 Shell 菜单中增设 `CONFIG_SHELL_CONSOLE`**

在 `Kconfig` 的 `menu "Shell"` 块内（约第 385 行后）添加配置项：
```kconfig
config SHELL_CONSOLE
	bool "Enable console TTY shell session"
	depends on SHELL
	default y
	help
	  在默认 console TTY 上启动独立 nshell 交互任务。如果禁用，
	  系统初始化时不自动派生 console 任务，但仍可通过 NLK 等通道派生 shell。
```

- [ ] **Step 2: 在 `app/shell/nshell.c` 中添加 `#ifdef CONFIG_SHELL_CONSOLE`**

修改 `app/shell/nshell.c` 末尾初始化逻辑：
```c
#ifdef CONFIG_SHELL_CONSOLE
static int nshell_init(void)
{
    return nshell_spawn(nos_console_tty(), "nshell", true);
}
task_init(nshell_init);
#endif
```

- [ ] **Step 3: 更新 `configs/nk60-v2_defconfig` 并精简任务栈**

在 `configs/nk60-v2_defconfig` 中更新 Shell 配置：
```ini
CONFIG_SHELL=y
CONFIG_SHELL_CONSOLE=n
CONFIG_SHELL_STACK_SIZE=1536
```

并在 `app/server/keyboard/keyboard_server.c` 中将 `keyboard_server` 栈大小从 4096 改为 2048：
```c
    ks->task = task_create("keyboard_server", keyboard_server_task_entry,
                           ks, 3, 2048, 10, NULL);
```

在 `app/server/nlk/nlk_server.c` 中将 `nlk_server` 栈大小从 4096 改为 3072：
```c
    g_nlk_srv.task = task_create("nlk_server", nlk_server_task_entry,
                                 &g_nlk_srv, 6, 3072, 10, NULL);
```

- [ ] **Step 4: 编译验证与 QEMU 回归**

运行构建命令确保零警告：
```bash
cmake --preset nk60-v2 && cmake --build --preset nk60-v2
cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32
python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf
```
预期：全部编译成功，`qemu-stm32` 冒烟测试全通过。

- [ ] **Step 5: 提交任务更改**

```bash
git add Kconfig app/shell/nshell.c configs/nk60-v2_defconfig app/server/keyboard/keyboard_server.c app/server/nlk/nlk_server.c
git commit -m "Kernel: Reclaim ~7KB heap by disabling UART console shell and tuning task stacks on NK60-V2"
```

---

### Task 2: Profile 存储免动态分配改造 (Phase 1)

**Files:**
- Modify: `app/server/keyboard/keyboard_server.c:192-315` (重构 `keyboard_config_save_profile` 与 `keyboard_config_load_profile`)

**Interfaces:**
- Produces: `int keyboard_config_save_profile(uint8_t slot_id)`, `int keyboard_config_load_profile(uint8_t slot_id)`
- Consumes: `ks->kb_cap`, `nos_open`, `nos_write`, `nos_read`, `nos_close`

- [ ] **Step 1: 编写 Profile 流式持久化头部与分块读写逻辑**

在 `app/server/keyboard/keyboard_server.c` 中，重写 `keyboard_config_save_profile`：
直接利用 RAM 中已有的 `ks->kb_cap` 数据，分块计算 CRC32 并流式写入，消除 `kmalloc(sizeof(struct keyboard_stored_profile))`：
```c
struct keyboard_profile_header {
    uint32_t magic;
    uint32_t version;
    uint8_t slot_id;
    uint8_t key_count;
    uint8_t layer_count;
    uint8_t active_layout_id;
    struct nlk_kb_params params;
    struct nlk_kb_light_cfg light;
    struct nlk_kb_zone_light_cfg zone_cfgs[NLK_KB_MAX_LIGHT_ZONES];
} __attribute__((packed));
```

在保存时：
1. 组装小尺寸的 `struct keyboard_profile_header`（约 150 字节栈内存）；
2. 循环计算 CRC32：Header -> 各 Layer 按键映射 -> 各 Layer Action Chains；
3. `nos_open` 打开目标文件；
4. 顺序写入 Header、各层 `keymap`、各层 `action_chains`、CRC32；
5. `nos_close(fd)` 完成写盘。

- [ ] **Step 2: 重写 `keyboard_config_load_profile` 为分块校验读取**

在 `keyboard_config_load_profile` 中：
1. 读取 `struct keyboard_profile_header`，校验 `magic` 与 `version`；
2. 循环读取并校验数据 CRC32；
3. 校验成功后就地应用至 `ks->kb_cap` 中；
4. 全程使用固定微小局部变量，0 字节动态 `kmalloc`。

- [ ] **Step 3: 编译与冒烟验证**

```bash
cmake --build --preset nk60-v2
cmake --build --preset qemu-stm32
python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf
```
预期：编译零告警，生成固件正常。

- [ ] **Step 4: 提交任务更改**

```bash
git add app/server/keyboard/keyboard_server.c
git commit -m "Keyboard: Eliminate 6.2KB heap allocation in profile save/load routines"
```

---

### Task 3: VFS `nos_sync` / `nos_fsync` 系统调用与接口契约实现 (Phase 2)

**Files:**
- Modify: `include/fs/fs.h:178-195` (声明 `nos_sync`, `nos_fsync`, `nos_fdatasync`)
- Modify: `include/fs/vfs.h:73-83` (`struct file_operations` 增加 `fsync`)
- Modify: `fs/vfs_io.c:1-103` (实现 `nos_sync`, `nos_fsync`, `nos_fdatasync`)
- Modify: `fs/fatfs/fatfs_vfs.c:550-565` (实现 FatFs `fsync` 与 `sync_fs`)

**Interfaces:**
- Produces: `nos_sync(void)`, `nos_fsync(int fd)`, `nos_fdatasync(int fd)`
- Consumes: `file->f_op->fsync`, `sb->s_op->sync_fs`

- [ ] **Step 1: 在 `include/fs/fs.h` 增加系统调用声明**

```c
/* VFS Synchronization API */
int nos_sync(void);
int nos_fsync(int fd);
int nos_fdatasync(int fd);
```

- [ ] **Step 2: 在 `include/fs/vfs.h` 更新 `file_operations`**

```c
struct file_operations {
  ssize_t (*read)(struct file *, char *, size_t, loff_t *);
  ssize_t (*write)(struct file *, const char *, size_t, loff_t *);
  int (*readdir)(struct file *, void *dirent, filldir_t filldir);
  int (*open)(struct inode *, struct file *);
  int (*release)(struct inode *, struct file *);
  int (*fsync)(struct file *, loff_t, loff_t, int);
  int (*same_file)(struct file *, struct file *);
  loff_t (*llseek)(struct file *, loff_t, int);
};
```

- [ ] **Step 3: 在 `fs/vfs_io.c` 实现 `nos_fsync` 与 `nos_sync`**

```c
int nos_fsync(int fd)
{
    struct file *file;
    int ret = 0;

    file = vfs_fd_get(fd);
    if (file == NULL) return -EBADF;

    if (file->f_op != NULL && file->f_op->fsync != NULL) {
        ret = file->f_op->fsync(file, 0, (loff_t)-1, 0);
    } else if (file->f_path.dentry != NULL &&
               file->f_path.dentry->d_sb != NULL &&
               file->f_path.dentry->d_sb->s_op != NULL &&
               file->f_path.dentry->d_sb->s_op->sync_fs != NULL) {
        ret = file->f_path.dentry->d_sb->s_op->sync_fs(file->f_path.dentry->d_sb, 1);
    }

    (void)vfs_fd_put(file);
    return ret;
}

int nos_fdatasync(int fd)
{
    return nos_fsync(fd);
}
```

实现 `nos_sync(void)` 遍历已挂载超级块：
```c
int nos_sync(void);
```

- [ ] **Step 4: 在 `fs/fatfs/fatfs_vfs.c` 接入 FatFs 刷盘操作**

实现 `fatfs_file_fsync` 调用 `f_sync(fp)` 并触发 `disk_ioctl(0, CTRL_SYNC, NULL)`；
实现 `fatfs_sync_fs` 调用 `sync_fs(fs)` 并注册在 `fatfs_super_ops` 中。

- [ ] **Step 5: 单元测试与验证**

编写测试文件 `tests/test_vfs_sync.c` 或在 `fs/vfs_smoke.c` 中加入 `nos_fsync` 与 `nos_sync` 调用测试，验证返回 0。
运行测试：
```bash
cmake --build --preset qemu-stm32
python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf
```

- [ ] **Step 6: 提交任务更改**

```bash
git add include/fs/fs.h include/fs/vfs.h fs/vfs_io.c fs/fatfs/fatfs_vfs.c
git commit -m "VFS: Implement nos_sync and nos_fsync system calls adhering to POSIX specification"
```

---

### Task 4: 通用 Block Cache 模块实现与 FatFs 桥接 (Phase 2)

**Files:**
- Create: `include/drivers/block_cache.h`
- Create: `drivers/block/block_cache.c`
- Modify: `drivers/block/sources.cmake` (添加 `block_cache.c`)
- Modify: `Kconfig` (添加 `CONFIG_BLOCK_CACHE` 与 `CONFIG_BLOCK_CACHE_SECTORS`)
- Modify: `fs/fatfs/diskio.c:1-150` (接入 Block Cache 读写与 `CTRL_SYNC`)

**Interfaces:**
- Produces: `struct block_cache`, `block_cache_init()`, `block_cache_read()`, `block_cache_write()`, `block_cache_sync()`
- Consumes: `block_read()`, `block_write()`

- [ ] **Step 1: 编写 `include/drivers/block_cache.h` 接口契约**

定义通用 Block Cache 结构体、缓存条目、状态标志位与 API。

- [ ] **Step 2: 在 `drivers/block/block_cache.c` 实现双模 Block Cache**

实现：
- 静态预分配或池化初始化；
- 扇区查找、LRU 淘汰置换、写合并（写入即更新 RAM 并标记 Dirty，不写 Flash）；
- `block_cache_sync` 顺序回写物理扇区并提交。

- [ ] **Step 3: 在 `fs/fatfs/diskio.c` 桥接 Block Cache**

当 `dev` 启用了 Block Cache 时：
`disk_read` 走 `block_cache_read`；
`disk_write` 走 `block_cache_write`；
`disk_ioctl(CTRL_SYNC)` 调用 `block_cache_sync`。

- [ ] **Step 4: 编译与跨板构建验证**

```bash
cmake --preset nk60-v2 && cmake --build --preset nk60-v2
cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32
python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf
```

- [ ] **Step 5: 提交任务更改**

```bash
git add include/drivers/block_cache.h drivers/block/block_cache.c drivers/block/sources.cmake Kconfig fs/fatfs/diskio.c
git commit -m "Drivers/Block: Implement generic dual-profile Block Cache with write-back and sync capabilities"
```

---

### Task 5: 引入并适配 LittleFS 文件系统 (Phase 3)

**Files:**
- Create: `libs/third_party/littlefs/lfs.h`, `lfs.c`, `lfs_util.h`, `lfs_util.c`
- Create: `fs/littlefs/littlefs_vfs.c`
- Modify: `fs/CMakeLists.txt`
- Modify: `fs/Kconfig`
- Modify: `board/nk60-v2/flash.c` (支持 LittleFS 挂载到 `/assets`)

**Interfaces:**
- Produces: `littlefs` 文件系统驱动，注册到 NOS VFS。
- Consumes: `w25q_flash` 物理接口。

- [ ] **Step 1: 集成 LittleFS 核心代码到 `libs/third_party/littlefs/`**

引入 ARM 官方轻量级 LittleFS v2 源码，配置标准 `lfs_util.h` 适配内核 `kmalloc/kfree/printk`。

- [ ] **Step 2: 编写 `fs/littlefs/littlefs_vfs.c` 实现 NOS VFS 桥接**

实现 `file_system_type littlefs_fs_type`：
- `mount`, `read`, `write`, `readdir`, `create`, `unlink`, `rename`, `fsync` 等接口；
- `fsync` 映射至 `lfs_file_sync()`；
- `sync_fs` 映射至 `lfs_sync()`。

- [ ] **Step 3: 在 `board/nk60-v2/flash.c` 支持配置优先挂载 LittleFS 到 `/assets`**

在 `CONFIG_ASSETS_FS_LITTLEFS` 开启时，直接将 W25Q assets 分区通过 LittleFS 挂载至 `/assets`。

- [ ] **Step 4: 编译与功能回归**

```bash
cmake --preset nk60-v2 && cmake --build --preset nk60-v2
```
验证固件编译成功，ROM 占用在 240KB 安全限额以内。

- [ ] **Step 5: 提交任务更改**

```bash
git add libs/third_party/littlefs/ fs/littlefs/ fs/CMakeLists.txt fs/Kconfig board/nk60-v2/flash.c
git commit -m "FS: Integrate LittleFS and adapt as the primary Flash filesystem on NOS VFS"
```

---

### Task 6: 键盘服务配置异步防抖与锁隔离 (Phase 4)

**Files:**
- Modify: `app/server/keyboard/keyboard_server.c:500-530, 900-940, 1060-1120`

**Interfaces:**
- Produces: 无阻塞按键矩阵扫描，异步防抖 Flash 写盘机制。
- Consumes: `nos_sync()`, `nlk_cap_keyboard_tick()`.

- [ ] **Step 1: 锁粒度解耦与 RPC 内存化快速响应**

修改 `ks_on_config_files`：
对改灯、改键、调参的 RPC，仅在 `ks->lock` 保护下快速更新 RAM，立即释放 `ks->lock` 并向上位机返回 ACK（时延 < 1ms）。

- [ ] **Step 2: 引入 1000ms 写回防抖定时器与快照机制**

维护 `config_dirty` 标志与 `sync_deadline_ms`。
在防抖到期时，微秒级拷贝当前配置快照，并在无锁状态下执行安全写盘与 `nos_sync()`。

- [ ] **Step 3: 核心按键扫描完全零阻塞保证**

确保 `keyboard_server_entry` 的 1ms 按键扫描主循环再也不会被写盘 I/O 挂起。

- [ ] **Step 4: 编译与完整回归验证**

```bash
cmake --preset nk60-v2 && cmake --build --preset nk60-v2
cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32
python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf
```

- [ ] **Step 5: 提交任务更改**

```bash
git add app/server/keyboard/keyboard_server.c
git commit -m "Keyboard: Decouple ks->lock from flash writes and implement asynchronous write-back debounce"
```

---

### Task 7: 全系统集成回归与固件验证

**Files:**
- Test: `tests/run_host_regressions.py`
- Test: `tests/qemu_stm32_smoke.py`
- Verify: `build/nk60-v2/nos.elf` (固件尺寸与符号表分析)

- [ ] **Step 1: 执行所有主机回归与架构规则检查**

```bash
python3 tests/run_host_regressions.py
python3 -m unittest discover -s tests -p 'test_*.py'
cmake -P tests/test_arch_flags.cmake
cmake -P tests/test_soc_headers.cmake
python3 scripts/check_layer_deps.py
```
预期：所有架构与主机回归检查 100% 通过。

- [ ] **Step 2: 四大板级预设全量构建**

```bash
cmake --preset nk60-v2 && cmake --build --preset nk60-v2
cmake --preset qemu-stm32 && cmake --build --preset qemu-stm32
cmake --preset wl-001 && cmake --build --preset wl-001
cmake --preset zj-v3 && cmake --build --preset zj-v3
```
预期：全部成功生成目标二进制固件。

- [ ] **Step 3: QEMU 运行冒烟**

```bash
python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf
```
预期：PASS。

- [ ] **Step 4: 提交最终集成版本**

```bash
git commit --allow-empty -m "Release: Complete memory reclamation, block cache, LittleFS, and flash write debounce"
```
