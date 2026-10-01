# NOS 单体内核重构 Implementation Plan

> **2026-10-01 状态说明：** 本文件为历史实施计划，已有 M1–M4 相关代码提交；下方未勾选框不能解释为所有任务尚未实施，也不能据提交存在就判断验收通过。当前设计与后续工作包见 [收口设计第 10–11 节](../specs/2026-10-01-nos-refactor-design.md#10-后续实施工作包与回退)。原代码片段仅作历史背景，尤其不要继续沿用调度前执行 APPLICATION、挂载时自动格式化、未确认后端的 smoke 或 `fs_open` 示例。后续补丁级计划应基于 R0–R4 重新展开。

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `master` 上按绞杀式演进完成第一阶段重构：arch/soc/board 分层、Zephyr 风格静态设备模型、统一 block + 重整 VFS/FatFs，金标板 `qemu-stm32` 可编可跑并通过 VFS 文件用例。

**Architecture:** 保持单体内核；上层只依赖下层契约。`arch` 提供 CPU 原语，`soc` 承载 STM32 寄存器/时钟，`board` 只做绑定与内存图，`drivers/base` 提供 `DEVICE_DEFINE` + init level，`fs/vfs` 为应用唯一 FS 入口，FatFs 经 block API 访问 ramdisk（金标）或 SPI-SD（可选）。未迁移板/驱动进 `legacy/`，默认不链入金标。

**Tech Stack:** C11 freestanding、GNU Make、现有 key=value/Kconfig 混合配置、`arm-none-eabi-`、QEMU STM32、ChaN FatFs（现有树内版本）。

**Spec:** `docs/superpowers/specs/2026-07-19-nos-refactor-design.md`

**金标验收命令（贯穿全程）：**

```bash
make qemu-stm32_config
make -j$(nproc)
make qemu-run
```

预期：串口/半主机日志出现 Welcome/调度相关输出；各里程碑另有附加日志断言。

---

## File structure（锁定分解）

| 路径 | 职责 |
|------|------|
| `include/kernel/device.h` + `drivers/base/device.c` | 新设备模型核心（演进现有结构） |
| `include/kernel/init.h` | `SYS_INIT` / init level；保留 `task_init` 兼容宏一段时间 |
| `include/drivers/serial.h` | UART 子系统 API |
| `include/drivers/block.h` | block 子系统 API |
| `drivers/serial/uart_stm32.c` | STM32 UART 驱动（经 soc 头访问寄存器） |
| `drivers/block/ramdisk.c` | 金标块设备 |
| `soc/stm32f1/` | 从 `board/qemu-stm32`（及同类 F1 板）抽出的 SoC 支持 |
| `board/qemu-stm32/` | 金标板：静态设备表、mm、ld、启动 |
| `arch/arm/include/asm/*.h` + `arch/arm/cpu/` | arch 契约；去掉对 `board/board.h` 的依赖 |
| `arch/arm64/include/asm/*.h` | 契约桩（编译可选、不要求可启动） |
| `fs/vfs.c` + `include/fs/*.h` | 整理现有 VFS 边界 |
| `fs/fatfs/diskio.c` | 仅调用 block API |
| `legacy/` | 未迁移板/驱动/应用 |
| `init/main.c` | 统一启动链：early → mm → device_init → sched → vfs mount |

---

## Milestone M1 — Arch / 构建 / 启动

### Task 1: 建立依赖门禁（kernel 不得含 board SoC 头）

**Files:**
- Create: `scripts/check_layer_deps.sh`
- Modify: `.gitlab-ci.yml`（可选，金标 job 调用脚本）

- [ ] **Step 1: 写入依赖检查脚本**

```bash
#!/usr/bin/env bash
# scripts/check_layer_deps.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
fail=0
if rg -n '#include\s*[<"]board/board\.h[>"]|#include\s*[<"]stm32f1|#include\s*[<"]stm32f4' \
  "$ROOT/kernel" --glob '*.c' --glob '*.h' ; then
  echo "ERROR: kernel/ must not include board/STM32 headers"
  fail=1
fi
if rg -n '#include\s*[<"]board/board\.h[>"]' \
  "$ROOT/arch/arm/cpu" --glob '*.c' --glob '*.h' ; then
  echo "ERROR: arch CPU code must not include board/board.h"
  fail=1
fi
exit "$fail"
```

- [ ] **Step 2: 跑脚本记录当前违规基线**

Run: `bash scripts/check_layer_deps.sh ; echo exit:$?`  
Expected: 当前会 FAIL（`arch/arm/cpu/cortex-m3/cpu_c.c` 等含 `board/board.h`）。把输出保存到笔记，本 Task 只加脚本，不修代码。

- [ ] **Step 3: Commit**

```bash
git add scripts/check_layer_deps.sh
git commit -m "build: add layer dependency check script for refactor"
```

### Task 2: 补齐 arch early init 契约并接入启动链

**Files:**
- Modify: `arch/arm/include/asm/init.h`
- Modify: `arch/arm/init.c`
- Modify: `include/kernel/board.h`（已有 early/late 声明则对齐实现）
- Modify: `board/qemu-stm32/init.c`
- Modify: `init/main.c`
- Create if missing: `board/qemu-stm32/include/board/init.h` 中的 early/late 弱实现或实实现

- [ ] **Step 1: 扩展 arch 契约头**

在 `arch/arm/include/asm/init.h` 增加：

```c
void arch_early_init(void);
void arch_init(void);
```

（保留现有 `asm_cpu_*` 声明若仍经由本头或 `cpu.h` 导出。）

- [ ] **Step 2: 实现空/薄 `arch_early_init`**

`arch/arm/init.c`:

```c
#include <kernel/kernel.h>
#include <asm/cpu.h>

void arch_early_init(void)
{
    /* Cortex-M: vector/clock early work stays in Reset_Handler / board for M1;
     * this hook exists so nos_start ordering is arch-portable. */
}

void arch_init(void)
{
    asm_cpu_init();
}
```

若 `asm_cpu_init` 已在别处调用，避免双调：以 `nos_start` 单一路径为准，删除重复调用点。

- [ ] **Step 3: 板级 early/late**

`board/qemu-stm32/init.c`:

```c
#include <kernel/kernel.h>
#include <kernel/board.h>
#include "board.h"

void board_early_init(void)
{
    /* clocks/gpio needed before console — call existing clk helpers if any */
}

void board_init(void)
{
    board_mm_init();
}

void board_late_init(void)
{
}
```

- [ ] **Step 4: 重排 `nos_start`**

`init/main.c` 启动序改为（VFS mount 可暂留，M3 再整理）：

```c
int nos_start(void)
{
    arch_early_init();
    board_early_init();
    cpu_init();          /* 若与 asm_cpu_init 重复则删减到只保留一层 */
    arch_init();
    board_init();
    console_init();
    kernel_log_init();
    nos_print_kernel_info();
    mm_node_early_init();
    mm_init();
    pid_init();
    sch_init();
    /* vfs_init + mount: keep current behavior until M3 */
    /* ... existing vfs block ... */
    sch_start();
    return 0;
}
```

注意：对照当前文件完整合并，勿丢掉 `vfs_init` 已有逻辑。

- [ ] **Step 5: 金标构建**

Run:

```bash
make qemu-stm32_config && make -j$(nproc)
```

Expected: 链接成功，生成 `out/nos.elf`。

- [ ] **Step 6: QEMU 冒烟**

Run: `make qemu-run`  
Expected: 日志含 `Welcome to NOS` 且不在 `nos_start` 早期空转死机。

- [ ] **Step 7: Commit**

```bash
git add arch/arm/include/asm/init.h arch/arm/init.c board/qemu-stm32/init.c init/main.c
git commit -m "init: order arch/board early init for portable boot chain"
```

### Task 3: 切断 arch CPU 对 `board/board.h` 的依赖

**Files:**
- Modify: `arch/arm/cpu/cortex-m3/cpu_c.c`
- Modify: `arch/arm/cpu/cortex-m4/cpu_c.c`（若金标不用 M4，仍一并改以免门禁失败）
- Create: `arch/arm/include/asm/cmsis.h` 或改用 soc 头（M1 若 `soc/` 尚未建，临时用 `arch/arm/include/asm/platform.h` 声明所需符号）

- [ ] **Step 1: 分析 `cpu_c.c` 从 board 取用了什么**

Run: `rg -n 'board\.h|STM32|GPIO|USART|SysTick' arch/arm/cpu/cortex-m3/cpu_c.c`  
记录每个符号来源。

- [ ] **Step 2: 最小替换**

若仅需 CMSIS/core 寄存器：改为 `#include <asm/...>` 下的 CMSIS 头（已有 `core_cm3` 路径），删除 `#include <board/board.h>`。  
若需要板级时钟频率之类：新增弱符号或 `extern uint32_t SystemCoreClock` 由 board/soc 提供，arch 只声明：

```c
/* arch/arm/include/asm/platform.h */
#pragma once
#include <kernel/types.h>
extern uint32_t SystemCoreClock;
```

- [ ] **Step 3: 跑依赖脚本**

Run: `bash scripts/check_layer_deps.sh`  
Expected: arch CPU 路径不再报 `board/board.h`（kernel 路径本就应干净）。

- [ ] **Step 4: 金标构建 + qemu-run**

Expected: 同 Task 2。

- [ ] **Step 5: Commit**

```bash
git add arch/arm/cpu arch/arm/include/asm
git commit -m "arch/arm: remove board.h dependency from Cortex-M CPU code"
```

### Task 4: 创建 `soc/stm32f1` 骨架并让 qemu-stm32 引用

**Files:**
- Create: `soc/Makefile`
- Create: `soc/stm32f1/Makefile`
- Create: `soc/stm32f1/include/soc/stm32f1xx.h`（可先做包装头，include 现有 `inside-include` 或逐步搬迁）
- Modify: top-level `Makefile` `dir-y` 增加 `soc`（在 `board` 前）
- Modify: `board/qemu-stm32/Makefile` / `Makefile.config`：去掉重复编译的 SPL 文件，改为依赖 `soc/stm32f1`

- [ ] **Step 1: 列出 qemu-stm32 中 ST 库与 arch_* 文件**

Run: `ls board/qemu-stm32/drivers board/qemu-stm32/*.c`

- [ ] **Step 2: `git mv` 可共享的 F1 支撑进 `soc/stm32f1/`**

优先搬：与板无关的 `system_stm32f10x.c`、`drivers/src|inc` 中通用外设库。  
保留在 board：`start.S`、`board_mm.c`、`arch_console.c`、`nos_kernel.ld.S`、`board_config.c`。

- [ ] **Step 3: 顶层接入**

`Makefile`:

```make
dir-y := init
dir-y += arch
dir-y += soc
dir-y += drivers
dir-y += kernel
dir-y += libs
dir-y += modules
dir-y += board
dir-y += app
dir-y += fs
```

`soc/Makefile`:

```make
obj-$(CONFIG_SOC_STM32F1) += stm32f1
```

在 `arch/arm/config/qemu-stm32_config` 或对应 configs 中增加 `CONFIG_SOC_STM32F1=y`。

- [ ] **Step 4: 修复 include 路径**

`scripts/Makefile.config` 或 `soc/stm32f1/Makefile` 增加：

```make
INC_DIR += -Isoc/stm32f1/include
INC_DIR += -Isoc/stm32f1/inside-include
```

- [ ] **Step 5: 金标全量编译**

Expected: `out/soc/stm32f1/built-in.o` 存在且最终链接成功。

- [ ] **Step 6: Commit**

```bash
git add Makefile soc board/qemu-stm32 arch/arm/config arch/arm/configs scripts
git commit -m "soc: extract stm32f1 support from qemu-stm32 board"
```

### Task 5: M1 收口 — 链接入口与文档

**Files:**
- Modify: `scripts/Makefile.config`（`Reset_Handler` 保留作 Cortex-M 默认可接受；注释标明 arch/board 可覆盖）
- Modify: `readme.md`（金标构建说明）

- [ ] **Step 1: 更新 readme 金标路径**

```markdown
## Build (golden board)

```bash
make qemu-stm32_config
make
make qemu-run
```
```

- [ ] **Step 2: 全量验证**

`make clean && make qemu-stm32_config && make && make qemu-run`

- [ ] **Step 3: Commit**

```bash
git add readme.md scripts/Makefile.config
git commit -m "docs: document qemu-stm32 as golden board after M1 boot chain"
```

**M1 出口：** 金标可跑；arch CPU 无 `board.h`；`soc/stm32f1` 已链入。

---

## Milestone M2 — 设备框架 + UART console

### Task 6: 引入 init level 与 `SYS_INIT`

**Files:**
- Modify: `include/kernel/init.h`
- Modify: `include/kernel/section.h`（若需新 section 宏）
- Modify: 链接脚本 `board/qemu-stm32/nos_kernel.ld.S` 或 `arch/arm/kernel/*.ld.S`：增加 `.init_level_*` 段
- Modify: `init/main.c` 或新建 `kernel/init.c`：实现 `sys_init_run(level)`
- Modify: `init/init.c`：`task_init_call` 改为在 `APPLICATION` level 调用或逐步淘汰

- [ ] **Step 1: 定义 level 与宏**

`include/kernel/init.h` 增加：

```c
enum init_level {
    INIT_LEVEL_EARLY = 0,
    INIT_LEVEL_PRE_KERNEL_1,
    INIT_LEVEL_PRE_KERNEL_2,
    INIT_LEVEL_POST_KERNEL,
    INIT_LEVEL_APPLICATION,
    INIT_LEVEL_COUNT,
};

typedef int (*sys_init_fn_t)(void);

typedef struct {
    const char *name;
    sys_init_fn_t fn;
    enum init_level level;
} sys_init_entry_t;

#define SYS_INIT(fn, lvl) \
    __visible __attribute__((section(".sys_init." #lvl))) \
    const sys_init_entry_t __sys_init_##fn = { \
        .name = #fn, .fn = fn, .level = INIT_LEVEL_##lvl \
    }

void sys_init_run(enum init_level level);
```

（section 命名需与链接脚本一致；若工具链对 `#lvl` 拼接挑剔，改用数字后缀 section。）

- [ ] **Step 2: 链接脚本收集符号**

在 ld 中增加类似：

```
__sys_init_start = .;
KEEP(*(.sys_init*))
__sys_init_end = .;
```

- [ ] **Step 3: 实现 runner**

```c
/* kernel/init_levels.c */
extern const sys_init_entry_t __sys_init_start[];
extern const sys_init_entry_t __sys_init_end[];

void sys_init_run(enum init_level level)
{
    const sys_init_entry_t *e;
    for (e = __sys_init_start; e < __sys_init_end; e++) {
        if (e->level == level && e->fn) {
            int rc = e->fn();
            if (rc)
                pr_err("SYS_INIT %s failed: %d\r\n", e->name, rc);
        }
    }
}
```

- [ ] **Step 4: 在 `nos_start` 调用**

在 `mm_init` 之后、`sch_start` 之前：

```c
sys_init_run(INIT_LEVEL_PRE_KERNEL_1);
sys_init_run(INIT_LEVEL_PRE_KERNEL_2);
sys_init_run(INIT_LEVEL_POST_KERNEL);
/* create idle/main tasks ... */
sys_init_run(INIT_LEVEL_APPLICATION);
```

- [ ] **Step 5: 构建验证** — 增加一个测试 `SYS_INIT` 打印 "sys_init ok"

- [ ] **Step 6: Commit**

```bash
git add include/kernel/init.h kernel/init_levels.c init/main.c board/qemu-stm32/nos_kernel.ld.S arch/arm/kernel
git commit -m "kernel: add Zephyr-style SYS_INIT levels"
```

### Task 7: 演进 `struct device` 为静态 DEVICE_DEFINE

**Files:**
- Modify: `include/kernel/device.h`
- Modify: `drivers/base/device.c`
- Keep: `device_register` 作为兼容包装（写入 legacy 路径或内部链表）

- [ ] **Step 1: 扩展设备结构（保持旧字段兼容迁移期）**

```c
struct device {
    const char *name;
    const void *config;
    void *data;
    const void *api;
    struct device_ops ops;      /* legacy; new drivers may leave zeroed */
    void *priv;                 /* alias of data during transition */
    struct mutex mutex;
    struct list_head list;
    uint32_t ref_count;
    uint8_t ready;
    int init_res;
};

struct device *device_get_binding(const char *name);
int device_is_ready(const struct device *dev);

#define DEVICE_DEFINE(dev_id, dev_name, init_fn, cfg_ptr, data_ptr, api_ptr, level) \
    static int __device_init_##dev_id(void) { \
        extern struct device __device_##dev_id; \
        int rc = init_fn(&__device_##dev_id); \
        __device_##dev_id.init_res = rc; \
        __device_##dev_id.ready = (rc == 0); \
        if (rc == 0) device_register(&__device_##dev_id); \
        return rc; \
    } \
    SYS_INIT(__device_init_##dev_id, level); \
    struct device __device_##dev_id = { \
        .name = dev_name, \
        .config = cfg_ptr, \
        .data = data_ptr, \
        .api = api_ptr, \
    }
```

- [ ] **Step 2: 实现 `device_get_binding`** = 现有 `device_find_by_name` 的别名或薄封装。

- [ ] **Step 3: 单元式验证** — 在 `drivers/base` 或 board 增加假设备 `nulldev`，`SYS_INIT` 后 `device_get_binding("null")` 非 NULL。

- [ ] **Step 4: 金标构建 + qemu-run**，日志含设备 init。

- [ ] **Step 5: Commit**

```bash
git add include/kernel/device.h drivers/base/device.c
git commit -m "drivers: add DEVICE_DEFINE static device model"
```

### Task 8: UART 子系统 API + stm32 驱动 + console 切换

**Files:**
- Create: `include/drivers/serial.h`
- Create: `drivers/serial/Makefile`
- Create: `drivers/serial/uart_stm32.c`
- Modify: `drivers/Makefile`（确保 `serial` 编入）
- Modify: `kernel/console.c` / `board/qemu-stm32/arch_console.c`
- Modify: `board/qemu-stm32/board_config.c`（静态 UART config）

- [ ] **Step 1: API 头**

```c
/* include/drivers/serial.h */
#pragma once
#include <kernel/device.h>

struct uart_driver_api {
    int (*poll_out)(const struct device *dev, unsigned char c);
    int (*poll_in)(const struct device *dev, unsigned char *c);
};

static inline int uart_poll_out(const struct device *dev, unsigned char c)
{
    const struct uart_driver_api *api = dev->api;
    if (!device_is_ready(dev) || !api || !api->poll_out)
        return -1;
    return api->poll_out(dev, c);
}
```

- [ ] **Step 2: 实现 `uart_stm32.c`**

- config：`struct uart_stm32_config { USART_TypeDef *regs; ... };` 头文件来自 `soc/stm32f1`，**不是** `board/board.h`。
- `DEVICE_DEFINE(uart1, "UART1", uart_stm32_init, &uart1_cfg, &uart1_data, &uart_stm32_api, PRE_KERNEL_1);`
- 板级提供 `uart1_cfg` 常量（可放 `board/qemu-stm32/board_devices.c`）。

- [ ] **Step 3: console 改为 `device_get_binding("UART1")` + `uart_poll_out`**

删除 console 路径上对板级直接寄存器写的依赖（或板级 `arch_console` 改为调用 serial API）。

- [ ] **Step 4: 验证**

`make qemu-stm32_config && make && make qemu-run`  
Expected: `Welcome to NOS` 仍输出；启动日志可见 UART/`SYS_INIT` 顺序。

- [ ] **Step 5: Commit**

```bash
git add include/drivers/serial.h drivers/serial board/qemu-stm32 kernel/console.c
git commit -m "serial: route console through DEVICE_DEFINE UART driver"
```

### Task 9: 金标配置裁剪非必要驱动

**Files:**
- Modify: `arch/arm/config/qemu-stm32_config` 或 `arch/arm/configs/qemu-stm32_config`
- Modify: `drivers/Makefile` / `app/Makefile`：金标关闭 USB/LVGL/display 等

- [ ] **Step 1: 最小化 config**，保证 console + 调度 +（现有）VFS 仍开。

- [ ] **Step 2: 干净构建通过。**

- [ ] **Step 3: Commit**

```bash
git commit -am "config: slim qemu-stm32 golden config for refactor path"
```

**M2 出口：** console 经 device；`DEVICE_DEFINE`/`SYS_INIT` 工作；kernel 无 board SoC 泄漏。

---

## Milestone M3 — Block + VFS/FatFs 重整

### Task 10: Block 子系统 API + ramdisk

**Files:**
- Create: `include/drivers/block.h`
- Create: `drivers/block/ramdisk.c`
- Create: `drivers/block/Makefile`
- Modify: `drivers/Makefile`

- [ ] **Step 1: API**

```c
/* include/drivers/block.h */
#pragma once
#include <kernel/device.h>
#include <kernel/types.h>

struct block_driver_api {
    int (*read)(const struct device *dev, uint64_t lba, void *buf, size_t count);
    int (*write)(const struct device *dev, uint64_t lba, const void *buf, size_t count);
    int (*get_info)(const struct device *dev, uint32_t *sector_size, uint64_t *sector_count);
};
```

- [ ] **Step 2: ramdisk 实现**

- 静态数组或 `kmalloc` 在 init 时分配（如 256 KiB，512B 扇区）。
- `DEVICE_DEFINE(ramdisk0, "ramdisk0", ramdisk_init, ... POST_KERNEL);`

- [ ] **Step 3: 小测试任务**（`SYS_INIT` 或 app）：对 LBA0 写模式再读回。

- [ ] **Step 4: Commit**

```bash
git add include/drivers/block.h drivers/block
git commit -m "block: add ramdisk device for golden VFS path"
```

### Task 11: FatFs diskio 改为只走 block API

**Files:**
- Modify: `fs/fatfs/diskio.c`
- Modify: FatFs 配置/挂载参数（volume ↔ device 名映射）

- [ ] **Step 1: 删除对 `spi-sdcard.h` / `qemu_sd.h` 的硬依赖**

```c
#include <drivers/block.h>
#include <kernel/device.h>

static const struct device *fat_bdev;

void fatfs_set_block_device(const struct device *dev)
{
    fat_bdev = dev;
}

DSTATUS disk_initialize(BYTE pdrv)
{
    if (!fat_bdev)
        fat_bdev = device_get_binding("ramdisk0");
    if (!device_is_ready(fat_bdev))
        return STA_NOINIT;
    return 0;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, DWORD sector, UINT count)
{
    const struct block_driver_api *api = fat_bdev->api;
    if (api->read(fat_bdev, sector, buff, count))
        return RES_ERROR;
    return RES_OK;
}
/* disk_write / disk_ioctl similarly */
```

- [ ] **Step 2: 在 FatFs/VFS 注册路径调用 `fatfs_set_block_device(device_get_binding("ramdisk0"))`**

- [ ] **Step 3: 金标默认 mount FatFs on ramdisk**（需先 mkfs 或使用预置镜像内存）。若现有 `vfs_mount("fatfs"...)` 依赖真实 SD：改为：

1. `ramdisk` init  
2. FatFs `f_mkfs`（若卷空）  
3. `vfs_mount("fatfs", ...)`  

- [ ] **Step 4: 验证无 SD 头依赖**

Run: `rg -n 'spi-sdcard|qemu_sd' fs/fatfs/diskio.c`  
Expected: 无匹配。

- [ ] **Step 5: Commit**

```bash
git add fs/fatfs
git commit -m "fatfs: route diskio through block device API"
```

### Task 12: VFS 公开 API 收敛 + 金标文件用例

**Files:**
- Modify: `include/fs/fs.h` / `include/fs/vfs.h`
- Create: `app/vfs_smoke.c`（或 `kernel/test/vfs_smoke.c`）
- Modify: `init/main.c` mount 策略（失败不致死）

- [ ] **Step 1: 稳定封装**（若尚无 POSIX 包装则补最小集）

```c
/* include/fs/fs.h — ensure these exist and are the app entry */
int fs_open(const char *path, int flags);
ssize_t fs_read(int fd, void *buf, size_t n);
ssize_t fs_write(int fd, const void *buf, size_t n);
int fs_close(int fd);
```

映射到现有 VFS inode/file 操作；名称以树内已有为准，若已有 `open` 宏则复用并在计划执行时统一命名。

- [ ] **Step 2: smoke 测试**

```c
static int vfs_smoke(void)
{
    int fd = fs_open("/smoke.txt", /*O_CREAT|O_RDWR*/ 2);
    if (fd < 0) { pr_err("vfs_smoke open fail\r\n"); return -1; }
    fs_write(fd, "nos\n", 4);
    fs_close(fd);
    fd = fs_open("/smoke.txt", /*O_RDONLY*/ 0);
    char buf[8] = {0};
    fs_read(fd, buf, 4);
    fs_close(fd);
    if (buf[0] != 'n') { pr_err("vfs_smoke data mismatch\r\n"); return -1; }
    pr_info("vfs_smoke OK\r\n");
    return 0;
}
SYS_INIT(vfs_smoke, APPLICATION);
```

（flags 数值必须改用树内真实常量。）

- [ ] **Step 3: qemu-run 期望日志含 `vfs_smoke OK`**

- [ ] **Step 4: Commit**

```bash
git add include/fs fs app init
git commit -m "fs: golden VFS smoke test on ramdisk FatFs"
```

### Task 13: （可选）SPI-SD block 后端

**Files:**
- Create: `drivers/block/block_spi_sd.c`（包装现有 `spi-sdcard`）
- Config gate: `CONFIG_BLOCK_SPI_SD`

- [ ] 仅当金标 ramdisk 路径已绿后实施；默认 `n`。
- [ ] Commit: `block: optional SPI-SD backend behind config`

**M3 出口：** `diskio` 无 SD 硬绑；金标 `vfs_smoke OK`。

---

## Milestone M4 — 内核目录收敛 + legacy 隔离

### Task 14: 内核目录 `git mv` 收敛

**Files:**
- Move: `kernel/sch/*` → `kernel/sched/`（若 `sched/` 已存在则合并）
- Move: task 实现 → `kernel/task/`
- Move: `mutex.c`/`sem.c`/`msg_queue.c` → `kernel/ipc/`
- Move: `irq.c` → `kernel/irq/`
- Modify: `kernel/Makefile`、所有 `#include` 路径、公开头 `include/kernel/sch.h` → 保留兼容 wrapper：

```c
/* include/kernel/sch.h */
#pragma once
#warning "Use <kernel/sched.h>"
#include <kernel/sched.h>
```

- [ ] **Step 1: 搬家前全量构建确认绿**

- [ ] **Step 2: `git mv` + 修 Makefile**

- [ ] **Step 3: 构建修复直至金标绿**

- [ ] **Step 4: Commit**

```bash
git commit -am "kernel: consolidate sched/task/ipc/irq directories"
```

### Task 15: `current` 访问收口

**Files:**
- Modify: `include/kernel/task.h` / sched 头
- Modify: 散落的 `extern struct task_struct *g_current_task`（以实际符号名为准）

- [ ] **Step 1: 提供统一宏**

```c
struct task_struct *task_current(void);
#define current task_current()
```

- [ ] **Step 2: 替换金标路径上的直接全局访问（可用 rg 批量）**

- [ ] **Step 3: 构建 + Commit**

```bash
git commit -am "sched: accessor for current task (SMP-ready hook)"
```

### Task 16: 建立 `legacy/` 并搬离非金标板

**Files:**
- Create: `legacy/README.md`
- Move: `board/zj-v3` `board/wl-001` `board/nk60`（及无法编入金标的重驱动）→ `legacy/board/...`、`legacy/drivers/...`
- Modify: top `Makefile` / board 选择逻辑：仅扫描 `board/` 不扫描 `legacy/`
- Modify: `servers/` — 移入 `legacy/servers` 或在 README 标明未实现

- [ ] **Step 1: 写 `legacy/README.md`** 说明迁回条件（需 DEVICE_DEFINE + 无 board.h 泄漏）。

- [ ] **Step 2: `git mv` 非金标板**

- [ ] **Step 3: 确认 `make qemu-stm32_config && make` 仍绿；`zj-v3_config` 可暂时失败（文档标明）。

- [ ] **Step 4: Commit**

```bash
git commit -am "legacy: quarantine non-golden boards and unfinished servers"
```

### Task 17: arm64 契约桩对齐

**Files:**
- Create/align: `arch/arm64/include/asm/{init,cpu,irq,spinlock,types,barrier,task}.h`
- Create: `arch/arm64/init.c` 提供空 `arch_early_init`/`arch_init` 若缺失
- Modify: `arch/arm64/Makefile` 确保可被编进（即使无板）

- [ ] **Step 1: 对照 `arch/arm/include/asm/` 补齐同名头，函数体 stub 返回或空。**

- [ ] **Step 2: 不要求 `qemu-virt` 链接成功；若强行 `ARCH=arm64` 无 board，允许清晰 `#error`：“arm64 port incomplete”。

- [ ] **Step 3: Commit**

```bash
git commit -am "arm64: align asm contract stubs with arm port"
```

### Task 18: 门禁与 M4 收口

**Files:**
- Modify: `scripts/check_layer_deps.sh`（扩展：drivers 金标路径禁止 `board/board.h`，允许 `soc/`）
- Modify: `.gitlab-ci.yml`：`make qemu-stm32_config && make` + `bash scripts/check_layer_deps.sh`
- Modify: `docs/superpowers/specs/2026-07-19-nos-refactor-design.md` 状态改为「M1–M4 实现跟踪见 plan」

- [ ] **Step 1: 扩展检查 —— `drivers/serial`/`drivers/block`/`kernel`/`fs` 不得 `#include <board/board.h>`**

- [ ] **Step 2: CI 与本地均通过**

- [ ] **Step 3: 最终验收清单**

```bash
bash scripts/check_layer_deps.sh
make clean
make qemu-stm32_config && make -j$(nproc)
make qemu-run   # expect Welcome + vfs_smoke OK
```

- [ ] **Step 4: Commit**

```bash
git commit -am "ci: enforce golden-board layer rules after refactor M4"
```

**M4 出口：** 目录收敛、legacy 隔离、依赖门禁、金标 VFS smoke 全绿。

---

## 后续（本计划不做）

- ARM64 `qemu-virt` 可启动  
- 完整 Device Tree  
- USB/LVGL/键盘从 legacy 迁回  
- FatFs 大版本升级、procfs/sysfs 完备化  

---

## Spec coverage self-check

| Spec 要求 | Plan 任务 |
|-----------|-----------|
| 单体内核 + 分层目录 | File structure, Task 4/14/16 |
| arch 契约 / 启动链 | Task 2–3, 5 |
| soc 抽出 | Task 4 |
| DEVICE_DEFINE + init level | Task 6–7 |
| UART console via device | Task 8 |
| block + ramdisk | Task 10 |
| FatFs via block | Task 11 |
| VFS 金标用例 | Task 12 |
| legacy 隔离 | Task 16 |
| arm64 契约预留 | Task 17 |
| 金标 qemu-stm32 | 贯穿 Task 2–18 |
| 不做 DT/微内核/ARM64 启动 | 「后续」节 |

## Type/API consistency notes

- 设备查找统一：`device_get_binding` / `device_is_ready`
- 串口：`struct uart_driver_api` + `uart_poll_out`
- 块设备：`struct block_driver_api` + 设备名 `"ramdisk0"`
- Init：`SYS_INIT(fn, LEVEL)` + `sys_init_run`
- 启动序：`arch_early_init` → `board_early_init` → `arch_init` → `board_init` → mm → `sys_init_run(*)` → `sch_start`
