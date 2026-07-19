# NOS 单体内核重构设计

**日期：** 2026-07-19  
**状态：** 已评审（对话确认）  
**范围：** 第一阶段（骨架 + 设备框架 + 最小 VFS/block/FatFs）

---

## 1. 背景与问题

NOS 是面向 STM32 Cortex-M 的单体 RTOS。当前可用能力包括优先级调度、任务、buddy/kmalloc、mutex/sem/msgq、简易设备表与 FatFs；主战场为 ARM Cortex-M3/M4 + STM32F1/F4。

主要结构性问题：

- **Arch 抽象过薄**：工具链、启动向量、板级 SoC 头、驱动 GPIO/SPI 全面 STM32/Thumb 化；`arch/arm64` 与 `board/qemu-virt` 几乎为空壳。
- **板级即 SoC**：`board.h` 直接暴露 `stm32*.h`；F1 多板大量复制 ST 标准外设库。
- **双驱动模型**：可用的 `kernel/device` 与未落地的 `wk`/bus（i2c）并存；大量驱动 `#include <board/board.h>`。
- **无真正 VFS**：`fs/vfs` 等为空；FatFs `diskio` 硬绑 SPI-SD。
- **目录未收敛**：`sch` vs `sched`、空 `ipc/`/`task/`、空 `servers/`/`microkernel/` 造成认知负担。
- **构建锁死 AArch32**：`arm-none-eabi-`、`-mthumb` 等硬编码在公共 Makefile。

后期需要接入 ARM64 等架构，并具备可扩展的文件系统能力。本设计定义**第一阶段**重构目标与边界；ARM64 可启动、完整 Device Tree、微内核服务化等列为后续子项目。

---

## 2. 目标与非目标

### 2.1 已锁定决策

| 决策点 | 选择 |
|--------|------|
| 内核形态 | **A. 单体内核**（类似 Zephyr / RT-Thread） |
| 首要成功标准 | **C. 软件架构清晰优先**（允许阶段性牺牲部分板/驱动） |
| 金标验证板 | **`qemu-stm32`** |
| 第一阶段范围 | **骨架 + 最小 VFS/block/FatFs + 驱动框架大改（B+C）** |
| 参考形态 | **偏 Zephyr** |
| 板级描述 | **构建期静态设备表**（`DEVICE_DEFINE` 风格，不上完整 DT） |
| 落地路径 | **方案 1：原地绞杀式演进**（legacy 隔离，金标持续可验证） |

### 2.2 第一阶段目标

1. 固化 `arch` / `board` / `soc` 分层与依赖方向，内核不再依赖 STM32 头。
2. 收敛为**一套**偏 Zephyr 的静态设备模型与 init level。
3. 落地最小 VFS + block 抽象，FatFs 作为 FS 后端；金标用 ramdisk 验证读写。
4. 收敛内核目录（sched/task/ipc/mm/irq/timer）；用 `legacy/` 隔离未迁移代码。
5. `qemu-stm32` 金标：可编、可跑到调度 + console，并通过 VFS 文件用例。
6. 为 ARM64 预留契约头与构建钩子（不要求本阶段可启动）。

### 2.3 非目标（本阶段明确不做）

- 完整 Device Tree 工具链与 binding 生态
- 微内核 / `servers/*` 用户态服务拆分
- ARM64 `qemu-virt` 可启动镜像、SMP、完整 MMU
- USB / LVGL / 键盘等产品驱动完整迁移
- 完整 procfs/sysfs、页缓存、多分区复杂策略
- 全板（zj-v3 / nk60 / wl-001 等）同步可运行

---

## 3. 总体分层与目录

依赖方向（上层只依赖下层契约，禁止反向 `#include`）：

```
应用/测试 (app/)
        ↓
子系统 API：VFS / 串口 / 块设备 / … (include/)
        ↓
内核核心：sched / task / ipc / mm / timer / irq (kernel/)
        ↓
驱动框架：device + init-level + 子系统驱动 (drivers/)
        ↓
板级：时钟/内存/console/IRQ 路由 (board/<name>/)
        ↓
架构：上下文切换、中断原语、CPU 启动 (arch/<arch>/)
```

### 3.1 目标目录

| 路径 | 职责 |
|------|------|
| `arch/<arch>/` | 唯一允许出现 CPU 汇编/异常模型的地方；导出稳定 `<asm/*>` 契约 |
| `board/<board>/` | 板级 init、内存图、console UART、链接脚本；不再塞整棵 ST 库当“板级内核” |
| `soc/`（新建） | STM32F1/F4 等 SoC 支持从 board 抽出，供多板复用 |
| `kernel/{sched,task,ipc,mm,irq,timer}/` | 收敛现有散落文件与空目录 |
| `drivers/base/` | Zephyr 风格 `DEVICE_DEFINE`、init level、按名/句柄查找 |
| `drivers/{serial,block,…}/` | 子系统驱动；禁止直接 `#include <board/board.h>` 拿寄存器 |
| `fs/{vfs,fatfs}/` | VFS 为核心；FatFs 为 filesystem 驱动 |
| `legacy/`（过渡） | 暂未迁移的板/驱动/应用，默认不编进金标配置 |
| `servers/`、`microkernel/` | 本阶段不实现；移入 `legacy` 或文档标明未实现，避免误导 |

**金标配置：** 仅 `qemu-stm32` + `ARCH=arm` 必须全绿。其余板进 `legacy` 或 `CONFIG_*` 关闭。

---

## 4. Arch 契约与启动链

### 4.1 Arch 契约

每个 arch（先 `arm`，后续 `arm64`）必须提供：

| 契约 | 内容 |
|------|------|
| 启动 | `arch_early_init` / `arch_init`：CPU、异常向量、早期时钟源 |
| 上下文 | `stack_init`、`context_switch`（协作/抢占所需最小集） |
| 中断 | `arch_irq_lock` / `arch_irq_unlock`（统一现有 save/restore 语义与命名） |
| 原子/屏障 | `spinlock` 原语、`barrier` |
| 类型 | `asm/types.h`（指针宽度、字长） |
| 可选钩子（本阶段可空实现） | `arch_cpu_idle`、cache/MMU 桩，供 ARM64 后续填充 |

规则：`kernel/**` **禁止** `#include <board/board.h>` 或 STM32 头；只能走 `asm/` 与 `kernel/` 公共头。

### 4.2 Board / SoC 职责拆分

- **`soc/stm32f1/`（等）**：时钟树辅助、IRQ 号、外设基址、厂商启动辅助；从现有 `board/*/drivers` 抽出复用。
- **`board/qemu-stm32/`**：选 SoC、注册内存节点、绑定 console UART、提供链接脚本、调用 `soc`/`device` 静态表。
- 板级对外只暴露：`board_early_init` / `board_init`、`board_mm_init`、console hook——不再把 `stm32f10x.h` 通过 `board.h` 泄漏给全树。

### 4.3 统一启动链

```
Reset → arch_early_init → board_early_init
     → 清 BSS / 重定位（若需要）
     → kernel_mm_init → irq/timer 基建
     → device_init (按 INIT_LEVEL 升序)
     → sched_init → 创建 idle/main
     → sched_start（不再返回）
```

现有 `task_init` 链接段注册，迁移为 Zephyr 风格 init level：驱动用 `DEVICE_DEFINE` / `SYS_INIT`，应用入口用明确的 `main` 或 `APP_INIT`，避免任意 `.task` 段隐式启动难以排序。

### 4.4 构建与工具链

- `ARCH` 选择工具链前缀与 `arch/$(ARCH)/Makefile.config`（arm → `arm-none-eabi-`；arm64 预留 `aarch64-none-elf-`）。
- 去掉顶层硬编码 `-mthumb` / `Reset_Handler` 到 board/soc；金标板自己声明 CPU flags。
- 配置可继续用现有 key=value，但按 arch/board 分层片段；完整 Kconfig 列为后续，不阻塞本阶段。

### 4.5 ARM64 预留

`arch/arm64/` 补齐空实现的契约头与 Makefile 骨架（能被 `ARCH=arm64` 选中并明确“未完成”或编过最小桩），不要求 `qemu-virt` 跑起来。

---

## 5. 设备模型与驱动框架

### 5.1 核心对象

```c
struct device {
    const char *name;
    const void *config;   /* 只读：基址、IRQ、时钟等板级常量 */
    void *data;           /* 运行时状态 */
    const void *api;      /* 子系统 API 表，如 uart_driver_api */
    /* 状态位：ready / 失败原因等 */
};
```

- 用 `DEVICE_DEFINE` 风格宏（无真 DT）在编译期生成设备实例与 init 入口。
- **Init level（建议）：** `EARLY` → `PRE_KERNEL_1` → `PRE_KERNEL_2` → `POST_KERNEL` → `APPLICATION`。
- 查找：`device_get_binding(name)`；子系统另提供 typed helper。

### 5.2 子系统 API（第一阶段必做）

| 子系统 | API 要点 | 金标用途 |
|--------|----------|----------|
| `serial` / UART | `poll_out` / fifo / 可选 IRQ 回调 | console、printk |
| `block` | `read`/`write` 扇区、`get_info` | VFS → FatFs diskio |
| 设备基类 | `init` 返回 0/-errno；失败设备不可用 | 统一错误路径 |

SPI/I2C/USB/display：本阶段只定目录与 api 头占位；实体驱动可留 `legacy`。

### 5.3 与板级/SoC 的边界

- 设备 `config` 由 board 静态表填写；驱动只读 `config`。
- SoC 寄存器访问集中在 `soc/` 或明确的 `uart_stm32.c` 等文件，不得经 `board.h` 向全树泄漏。
- 旧 `device_register`：保留兼容包装仅供 legacy；金标路径不再依赖“驱动任务里自行 register”。

### 5.4 废弃与隔离

- 删除或移入 `legacy`：半截 `wk` device model、`of_device_id` 死代码、空的 `include/wk`。
- 未编入金标的驱动：能改则改接口，不能改则 `legacy` + 不链入 `qemu-stm32`。

### 5.5 错误处理

- 驱动 `init` 失败返回负错误码，框架标记 device 未就绪；仅编程错误使用 `BUG_ON`。
- 调用未就绪设备：返回 `-ENODEV` / `-EIO`，不静默写寄存器。

### 5.6 金标验证

- 启动日志可见按 level 的 init 顺序。
- `printk` 走新 UART device。
- 最小 `block` 后端优先 **RAM disk**（不依赖 SD）；SPI-SD 为可选第二后端。

---

## 6. VFS / block / FatFs

### 6.1 分层

```
app / shell 测试
    ↓  open/read/write/mount …
fs/vfs  （路径、挂载点、file/inode 操作表）
    ↓
fs 驱动（fatfs 适配层）
    ↓
block device API
    ↓
ramdisk（金标默认） / spi-sd（可选）
```

### 6.2 VFS 最小表面

| 能力 | 说明 |
|------|------|
| 挂载表 | `mount(fs_type, mnt_point, device_name, flags)` |
| 文件 API | `open/close/read/write/lseek/unlink/stat`（POSIX 子集） |
| 目录 API | `opendir/readdir/closedir`（FatFs 能支撑的先做） |
| 注册 | `FS_DEFINE("fatfs", &fatfs_ops)` 静态注册文件系统类型 |

不追求 Linux VFS 全量；结构上预留 `file_ops` / `fs_ops` 函数表。

### 6.3 Block 层

- `block_driver_api`：`read_blocks` / `write_blocks` / `get_sector_size` / `get_block_count`。
- 金标默认：`ramdisk`；启动时按配置 `mkfs`（可选）后 `mount("fatfs", "/", "ramdisk0")`。
- SPI-SD：另一 block 驱动；`fs/fatfs/diskio.c` **只**调用 block API。

### 6.4 FatFs 适配

- 保留 ChaN FatFs（可继续 R0.11；升级版本后续）。
- 薄适配：`disk_*` → NOS block；同步钩子 → NOS mutex。
- 应用侧：新代码禁止直接调 `f_open`；旧 `fatfs_test` 改为 VFS 或移入 `legacy`。

### 6.5 启动衔接

- `POST_KERNEL`：block + fatfs 适配 init。
- `APPLICATION`：按 config 自动 mount，或由测试任务显式 mount。
- mount 失败不阻止调度与 console。

### 6.6 金标验证标准

1. 无 SD：ramdisk + FatFs + VFS 完成创建/读写/关闭文件。
2. 可选：SPI-SD 启用时同一套 VFS 用例通过。
3. FatFs 头不泄漏给 app；公开头在 `include` / `fs/vfs` 下稳定。

---

## 7. 内核子系统整理与兼容

### 7.1 目录收敛

| 现状 | 目标 |
|------|------|
| `kernel/sch/` + 空 `sched/` | 统一为 `kernel/sched/` |
| `sch/task.c` + 空 `task/` | `kernel/task/` |
| IPC 散落根目录 + 空 `ipc/` | `kernel/ipc/` |
| `irq.c` + 空 `irq/` | `kernel/irq/` |
| 全局单 `current`、无 SMP | 保持 UP；`current` 访问收口到宏/内联 |

行为上优先保持调度语义（优先级位图、时间片）不变，先搬家与清边界。

### 7.2 Legacy 策略

- 金标 `qemu-stm32` 只走新路径。
- `legacy/`：未迁板、USB/LVGL/键盘等重驱动、直接调 FatFs 的测试；默认不进金标链接。
- API 破坏允许；破坏点写入迁移说明。
- 大搬家优先 `git mv`；功能删除需在提交说明写清。

---

## 8. 里程碑

| 里程碑 | 交付 | 出口标准 |
|--------|------|----------|
| **M1** Arch/构建/启动 | arch 契约、工具链按 ARCH、启动链、`qemu-stm32` 进 idle/main | 金标能编能跑到调度心跳 + printk |
| **M2** 设备框架 | `DEVICE_DEFINE` + init level；UART console 迁入 | kernel 无 `board.h` 泄漏；console 经 device |
| **M3** Block + VFS + FatFs | ramdisk、VFS API、FatFs 适配、金标读写文件 | VFS 用例通过；diskio 无 SD 硬绑 |
| **M4** 内核目录收敛 + legacy 隔离 | sched/task/ipc 归位；legacy 不污染金标 | 目录与依赖规则落地；本设计已入库 |
| **后续**（另开 spec） | ARM64 `qemu-virt` 启动、真 DT、更多驱动、procfs 等 | — |

执行顺序严格 **M1 → M2 → M3 → M4**。M3 金标只用 ramdisk；SD 为可选。

---

## 9. 风险与对策

| 风险 | 对策 |
|------|------|
| B+C 范围仍偏大 | 严格按里程碑；M3 金标只用 ramdisk |
| SoC 抽出时板级复制爆炸 | 先抽 F1 供 `qemu-stm32`；其它板整板进 legacy |
| Zephyr 风格与旧代码混用 | 金标禁止旧注册路径；兼容层仅服务 legacy |
| 空目录/`servers` 误导 | 文档标明未实现；移入 legacy 或删除空壳 |

---

## 10. 测试策略

- **构建门禁：** `make qemu-stm32_config && make`
- **运行验证：** QEMU 下检查启动日志、调度、VFS 文件用例（应用任务或脚本）
- **回归范围：** 本阶段不要求全板通过

---

## 11. 后续子项目（本设计范围外）

1. **ARM64 端口：** `arch/arm64` 完整实现 + `board/qemu-virt` 可启动（UP，MMU 按需分期）。
2. **Device Tree：** 在静态 `DEVICE_DEFINE` 形态稳定后引入 DTS/生成头。
3. **子系统驱动迁移：** UART 之外的 SPI/I2C/USB/display 从 legacy 迁回。
4. **VFS 扩展：** ramfs/procfs、多挂载点策略、FatFs 版本升级。
5. **配置系统：** 完整 Kconfig/menuconfig（可选）。

---

## 12. 参考

- 现有代码结构勘察（2026-07-19）：单体 Cortex-M、设备表、FatFs R0.11、arm64 空壳。
- 形态参考：Zephyr（device/init level、arch 分层）；VFS/block 接口保持嵌入式可实现的最小子集，不照搬 Linux。
