# NOS 内存管理子系统重构设计规范（多引擎并存 + MCU MPU + MMU VMM）

- **日期**：2026-11-01
- **模块**：
  - 核心分配器与物理节点：`include/kernel/mm.h`, `include/nos/mm.h`, `kernel/mm/mm_node.c`, `kernel/mm/mm_buddy.c`, `kernel/mm/mm_tlsf.c`, `kernel/mm/mm_slub.c`, `kernel/mm/mm_block.c`, `kernel/mm/mm.c`, `kernel/mm/mm_page.c`
  - MCU 硬件 MPU 与地址空间：`arch/arm/mm/mpu.c`, `arch/arm/hal/hal_arm.c`, `arch/arm/cpu/cortex-m3/cpu_c.c`, `arch/arm/cpu/cortex-m4/cpu_c.c`, `kernel/mm/vmm_mpu.c`, `kernel/task/task.c`, `kernel/sched/sched.c`
  - MMU 虚拟内存与多级页表：`kernel/mm/vmm_mmu.c`, `arch/arm64/hal/hal_arm64.c`, `arch/riscv/hal/hal_riscv.c`, `arch/x86/hal/hal_x86_64.c`, `include/nos/hal_arch.h`
  - 链接脚本与标准库对接：`board/*/nos_kernel.ld.S`, `libs/libc/port/picolibc_newlib.c`

---

## 1. 背景与现状痛点诊断

当前 NOS 内存管理子系统在低内存 MCU（20KB~128KB SRAM）与大内存 MMU 架构（ARM64 / RISC-V64 / x86_64）两端均存在严重瓶颈：

1. **链接脚本与节点初始化的页对齐截断浪费**：
   - `board/*/nos_kernel.ld.S` 在 `.data` 起始和 `.bss` 结尾强制执行 `. = ALIGN(CONFIG_PAGE_SIZE)`，在 `CONFIG_PAGE_SIZE=4096` 的板级上产生最高近 8KB 的对齐空洞。
   - `__mm_pool_end = _estack - _Min_Stack_Size` 减去 `0x200` 字节后非整页对齐，`mm_node_early_init()` 执行 `ALIGNED_PAGE_DONE(node->end)` 向下截断，直接丢弃末尾近 3.5KB 的可用 SRAM。
   - `libs/libc/port/picolibc_newlib.c` 中静态定义了 `uint8_t g_libc_sbrk_arena[16384]`（16KB BSS 数组），而 `malloc/free` 实际已走 `kmalloc/kfree`，导致 16KB RAM 被闲置吞噬。
2. **Buddy 分配器元数据膨胀与切分缺陷**：
   - 每个页面强制分配 `struct page`（24~40B）并向上按整页取整占用头部页；
   - `mm_buddy_init()` 要求高阶块按 `(1 << order) * PAGE_SIZE` 物理地址自然对齐，由于 `buddy_page_index` 偏移破坏了起始对齐，连续内存被过早切碎；
   - 将块内全部 `1 << order` 个 `struct page` 都挂入同一个链表，分裂与合并开销为 $O(2^{\text{order}})$。
3. **`mm_block.c` 二级分配器导致严重的内/外部碎片**：
   - 申请 `4096B`（如任务栈）加上 `sizeof(struct memblock) + sizeof(struct mem_base)` 后超过 1 页，`size_to_order()` 直接申请 2 页（8192B），造成近 50% 跨阶浪费；
   - 每个分配块带 24~32 字节 `struct mem_base` 头部且已分配块仍留在全局链表中，小对象膨胀率 200%~300%，分配查找退化为 $O(N_{\text{total}})$；
   - 大小对象、长短生命周期混排在同一 `memblock` 中，产生严重的 Pinning 外部碎片。
4. **MCU 硬件 MPU 支持缺失**：
   - Cortex-M3/M4 虽具备 PMSAv7 8-Region 硬件 MPU，但内核尚未启用，缺少 NULL 指针解引用硬件陷阱、Flash 只读保护、SRAM 不可执行（XN）保护、32B 硬件任务栈溢出护栏以及 `MemManage_Handler` 故障精确诊断。
5. **MMU 虚拟内存与多级页表未闭环**：
   - `arm64`/`riscv`/`x86_64` 的 `hal_vm_map`/`hal_vm_unmap` 仅做对齐校验并返回 0，缺乏真实的多级页表 Walk、VMA 虚拟区间管理、缺页按需分配（Demand Paging）与写时复制（CoW）。

---

## 2. 总体架构设计

采用 **“统一前端 API + 节点级多态分配引擎 + 双模地址空间（MPU / MMU）”** 的三层架构，借鉴 **Linux**（Zone Buddy + SLUB + VMA + 多级页表）与 **Zephyr / RT-Thread**（TLSF 二级位图紧凑堆 + PMSAv7 MPU 栈护栏与动态槽位）的核心设计：

```text
+---------------------------------------------------------------------------------------+
| 统一内存与地址空间前端 API (include/kernel/mm.h & include/nos/mm.h)                     |
|  - 内核堆 API: kmalloc / kzalloc / krealloc / kfree / kmalloc_aligned                  |
|                kalloc_by_pid / kfree_by_pid / kmem_cache_{create,alloc,free,destroy}  |
|  - 页帧 API:   alloc_pages / alloc_page / alloc_pages_exact / free_pages / free_page  |
|  - 地址空间:   nos_vm_space_{init,destroy} / nos_vm_{mmap,munmap,mprotect,fault}      |
+-------------------------------------------+-------------------------------------------+
                                            |
        +-----------------------------------+-----------------------------------+
        | 物理与内核内存分配层 (kernel/mm/)                                      |
        v                                                                       v
+---------------------------------------+               +---------------------------------------+
| 引擎 A: TLSF 紧凑边界标记引擎          |               | 引擎 B: Linux 风格 Buddy + SLUB 引擎   |
| (kernel/mm/mm_tlsf.c)                 |               | (mm_buddy.c + mm_slub.c)              |
| - 0 struct page 开销，0 页截断损耗     |               | - 标准伙伴系统（仅块首页入链 + 位图） |
| - 8B/16B 紧凑 Boundary Tag 块头       |               | - alloc_pages_exact 自动归还多余尾页   |
| - 空闲链表指针复用 Payload 区域       |               | - SLUB kmem_cache 页内 0 字节块头开销 |
| - O(1) FLI/SLI 位图查找 + 立即前后合并|               | - >2048B 大块分配直通 Buddy           |
+---------------------------------------+               +---------------------------------------+
        |                                                                       |
        +-----------------------------------+-----------------------------------+
                                            v
+---------------------------------------------------------------------------------------+
| 双模硬件内存保护与虚拟内存层 (kernel/mm/vmm_mpu.c & vmm_mmu.c + arch/*/mm/)            |
|  - 模式 1 (CONFIG_MPU=y): PMSAv7 8-Region (NULL 陷阱 + Flash RX + SRAM XN +           |
|                           32B 硬件任务栈溢出护栏 + 任务动态窗口 + MemManage 精确诊断)  |
|  - 模式 2 (CONFIG_MMU=y): VMA 区间管理 + 真实多级页表 (ARM64/Sv39/x86_64) +            |
|                           Demand Paging 按需分页 + Copy-on-Write (CoW) 引用计数       |
+---------------------------------------------------------------------------------------+
```

---

## 3. 阶段 1：物理内存节点与多引擎内核分配器设计

### 3.1 链接脚本与 Newlib `_sbrk` 零空洞优化
1. **MCU 链接脚本 (`board/{wl-001,nk60-v2,zj-v3,qemu-stm32,nutiny-nuc472}/nos_kernel.ld.S`)**：
   - 将 `.data` 段的 `. = ALIGN(CONFIG_PAGE_SIZE)` 改为 `. = ALIGN(8)`；
   - 将 `.bss` 结束后的 `__mm_pool_start` 对齐从 `ALIGN(CONFIG_PAGE_SIZE)` 改为：在 `CONFIG_MM_TLSF=y` 时仅 `. = ALIGN(8)`，在纯页级 Buddy 模式下保持页对齐；
   - 保留 `__mm_pool_end = _estack - _Min_Stack_Size`，在 TLSF 模式下按 8 字节向下对齐，使末尾非整页的数 KB 内存 100% 进入可分配池。
2. **Newlib `_sbrk` 桥接优化 (`libs/libc/port/picolibc_newlib.c`)**：
   - 移除静态 BSS 数组 `g_libc_sbrk_arena[16384]`；
   - `_sbrk_r` 按需向内核分配器申请或通过小型动态块服务，立即为所有 ARM MCU 板级省出 **16KB SRAM**。

### 3.2 多态物理内存节点 (`struct mm_node`)
扩展 `struct mm_node` 以支持节点级分配算法标识与多段 SRAM 共存：
```c
enum mm_node_algo {
    MM_NODE_ALGO_AUTO  = 0, /* 根据 Kconfig 默认选择 TLSF 或 BUDDY */
    MM_NODE_ALGO_TLSF  = 1, /* 字节级 TLSF 紧凑边界标记管理（0 struct page 开销） */
    MM_NODE_ALGO_BUDDY = 2, /* 标准页级 Buddy 伙伴系统管理（配合上层 SLUB/Block） */
};
```
- 当节点采用 `MM_NODE_ALGO_TLSF` 时：
  - `mm_node_early_init()` 不对 `node->start` / `node->end` 做 `CONFIG_PAGE_SIZE` 截断，仅做 `sizeof(addr_t)` (4B/8B) 对齐；
  - `node->buddy_page_index = 0`，不分配 `struct page` 数组，整个 `[node->start, node->end)` 直接初始化为一个 TLSF Arena；
  - `mm_get_total_page_num()` 与 `mm_get_free_page_num()` 根据 TLSF Arena 的总字节数与空闲字节数折算页数，保持 `procfs` 与 `nshell free` 输出语义完全一致。
- 当节点采用 `MM_NODE_ALGO_BUDDY` 时：
  - 初始化改进型标准 Buddy，并由上层 SLUB (`CONFIG_MM_SLUB=y`) 或兼容层提供 `kmalloc`。

### 3.3 引擎 A：TLSF 二级位图 + 紧凑边界标记分配器 (`kernel/mm/mm_tlsf.c`)

#### 3.3.1 块头与内存布局
每个物理块（Allocated 或 Free）拥有紧凑对齐的边界标记头 `struct tlsf_block_hdr`：
```c
#define TLSF_BLOCK_FREE_BIT     (1UL << 0)
#define TLSF_BLOCK_PREV_FREE    (1UL << 1)
#define TLSF_BLOCK_SIZE_MASK    (~3UL)

struct tlsf_block_hdr {
    struct tlsf_block_hdr *prev_phys; /* 前驱物理相邻块（仅当 PREV_FREE 置位时有效） */
    size_t size_flags;                /* 块 payload 大小 (对齐到 8B) | FREE_BIT | PREV_FREE */
    pid_t owner_pid;                  /* 所属任务 PID (支持 kalloc_by_pid / kfree_by_pid) */
#ifdef CONFIG_MM_DEBUG
    uint32_t magic;                   /* 调试魔数 0x544C5346 ('TLSF') */
#else
    uint16_t pad;
#endif
    /* 若为空闲块，紧随其后的 payload 起始处复用存放空闲双向链表指针：
     * struct tlsf_block_hdr *next_free;
     * struct tlsf_block_hdr *prev_free;
     */
};
```
- **开销对比**：32 位 MCU 下非调试模式已分配块头仅 **12~16 字节**（相比旧 `mem_base` 24B + `memblock` 跨页浪费，节省超过 60%），且**空闲链表指针完全复用空闲块 Payload**，已分配块无需挂入任何链表！
- **物理尾哨兵块（Sentinel Epilogue）**：在每个 TLSF Arena 末尾放置一个 `size = 0, FREE_BIT = 0` 的尾哨兵，消除边界分支判断。

#### 3.3.2 $O(1)$ 二级位图索引与原地合并
- **FLI（一级索引）**：覆盖 $2^4$ (16B) 到 $2^{24}$ (16MB)，共 21 个一级桶；
- **SLI（二级索引）**：每级 4 bits（8 或 16 个线性细分区间）；
- **分配流程 (`mm_tlsf_alloc_aligned`)**：
  1. 将请求的 `size` 向上取整到下一个二级桶，使用 `__builtin_ctz` 在 `fl_bitmap` 和 `sl_bitmap[fl]` 中 $O(1)$ 定位最小可用空闲块；
  2. 若要求大于 8B 的特殊对齐（如 MPU 栈护栏 32B 对齐或 `alloc_pages` 的 `CONFIG_PAGE_SIZE` 对齐），计算对齐缺口 `gap`：若前导 `gap >= MIN_BLOCK_SIZE`，将前导部分切分为独立空闲块挂回位图；
  3. 若切分后尾部剩余空间 $\ge$ 最小块阈值，切分尾块并挂回位图；
  4. 全程无循环遍历，时间复杂度严格 $O(1)$。
- **释放与立即合并 (`mm_tlsf_free`)**：
  1. 检查 `size_flags & TLSF_BLOCK_PREV_FREE`：若前驱物理块空闲，直接通过 `prev_phys` $O(1)$ 摘除前驱并向前合并；
  2. 通过 `(uintptr_t)payload + size` 定位后继物理块：若后继空闲，$O(1)$ 摘除后继并向后合并；
  3. 将合并后的大块一次性插入对应 `(fl, sl)` 空闲链表并置位位图。**相邻碎片残留率为 0%**。

### 3.4 引擎 B：标准 Linux 风格 Buddy (`kernel/mm/mm_buddy.c`) 与 SLUB (`kernel/mm/mm_slub.c`)

#### 3.4.1 Buddy 分配器改造
1. **仅块首页（Head Page）入链**：
   - 每个 `order` 空闲块仅将 `page[head_idx].list` 挂入 `buddy->info[order].list`；
   - `page[head_idx].private = order`，其余尾页 `private = -1`；
   - 分裂（Split）与合并（Coalesce）仅操作 1 个链表节点，时间复杂度从 $O(2^{\text{order}})$ 降至严格 $O(1)$。
2. **解除物理地址自然高阶对齐限制**：
   - 初始化时将可用区间按最大可能阶数贪心切分，不再因为 `buddy_page_index` 偏移而丢弃或切碎高阶连续块。
3. **精确页分配接口 `alloc_pages_exact(size_t size, gfp_t flag)` / `free_pages_exact(addr_t addr, size_t size)`**：
   - 当请求大小跨越非 2 的幂页数（如 3 页）时，从 Order 2（4 页）分配后，立即把多余的 1 页放回 Order 0 空闲链表，彻底解决大块跨阶内碎片。

#### 3.4.2 SLUB 小对象分配器 (`kernel/mm/mm_slub.c`)
1. **专用对象池与通用尺码池**：
   - 提供标准接口：
     ```c
     struct kmem_cache *kmem_cache_create(const char *name, size_t size, size_t align, uint32_t flags);
     void *kmem_cache_alloc(struct kmem_cache *cache, gfp_t flag);
     int kmem_cache_free(struct kmem_cache *cache, void *obj);
     void kmem_cache_destroy(struct kmem_cache *cache);
     ```
   - 内置 8 档通用 `kmalloc_caches`：`16, 32, 64, 128, 256, 512, 1024, 2048` 字节。
2. **页内零头部（Zero-Header Slab Page）**：
   - 当请求 `size <= 2048` 时，从对应 `kmem_cache` 的 Partial Slab 页中通过单向内嵌 `freelist` 指针 $O(1)$ 弹出一个槽位；
   - 释放 `kfree(addr)` 时通过 `find_mm_node(addr)` 找到所属 `struct page`：
     - 若该页标记为 Slab 页，直接归还给所属 `kmem_cache` 的页内 `freelist`；当整页 `inuse == 0` 时可将整页归还给 Buddy；
     - 若该页标记为大块页，直接归还给 Buddy。

---

## 4. 阶段 2：MCU 硬件 MPU (PMSAv7) 子系统设计

### 4.1 8-Region 硬件分区表 (`arch/arm/mm/mpu.c` & `kernel/mm/vmm_mpu.c`)

针对 ARMv7-M (Cortex-M3/M4) 的 8 个硬件 MPU Region，采用 **“5 静态全局区 + 3 动态上下文切换区”** 布局（高编号 Region 自动覆盖低编号 Region 的重叠区域）：

| Region | 用途 | 基址与大小 | 权限与属性 (`RASR`) | 说明 |
| :--- | :--- | :--- | :--- | :--- |
| **0** | **NULL 指针陷阱** | `0x00000000`, 64B | `AP=000` (No Access), `XN=1` | 任何特权/非特权代码读写或跳转到 `0x0~0x3F` 立即触发 `MemManage` |
| **1** | **Flash 代码常量区** | `0x08000000`, 1MB | `AP=110` (RO), `XN=0`, Normal WT | 保护内核 `.text`/`.rodata` 免遭野指针篡改 |
| **2** | **内部 SRAM 数据区** | `0x20000000`, 128KB | `AP=011` (RW), `XN=1`, Normal WBWA | 允许读写，硬件禁止在 SRAM 中取指执行（防栈溢出代码注入） |
| **3** | **外设 MMIO 区** | `0x40000000`, 512MB | `AP=011` (RW), `XN=1`, Device | 外设寄存器区强序访问、禁止执行 |
| **4** | **扩展外部内存区** | `0x60000000`, 256MB | `AP=011` (RW), `XN=1`, Normal | 覆盖 FSMC 外部 SRAM（如 `zj-v3` 的 `0x68000000`） |
| **5** | **动态任务栈溢出护栏** | `current->stack_guard`, **32B** | `AP=000` (No Access), `XN=1` | 覆盖在 Region 2 之上；位于当前任务栈最低 32 字节，栈溢出第一现场拦截 |
| **6** | **动态任务窗口 0** | `vm_space->slots[0]` | 由 `hal_vm_map` 编码 | 任务私有数据区或 IPC 共享缓冲区映射 |
| **7** | **动态任务窗口 1** | `vm_space->slots[1]` | 由 `hal_vm_map` 编码 | 用户态驱动授权 MMIO 窗口或附加内存段 |

### 4.2 数据结构与快速切换 (`struct nos_vm_space`)
```c
#define NOS_MPU_DYNAMIC_SLOTS   2U  /* Region 6 & Region 7 */
#define NOS_MPU_GUARD_REGION    5U  /* Region 5: 32B Stack Guard */
#define NOS_MPU_GUARD_SIZE      32U

struct nos_mpu_slot {
    uintptr_t base;
    size_t    size;
    uint32_t  prot;
    uint32_t  rbar;
    uint32_t  rasr;
};

struct nos_vm_space {
    uint32_t            mpu_region_mask;
    uint32_t            guard_rbar;
    uint32_t            guard_rasr;
    struct nos_mpu_slot slots[NOS_MPU_DYNAMIC_SLOTS];
};
```
- **32B 硬件栈护栏初始化**：
  - `alloc_task_stack()` 使用 `kmalloc_aligned(stack_size + 32, 32, GFP_KERNEL)` 分配 32B 对齐的内存块；
  - 底部 `[buf, buf + 32)` 设为 `task->stack_guard_base`，预编码 `task->vm_space.guard_rbar` 与 `guard_rasr`（32B, `AP=000`, `XN=1`, `ENA=1`）；
  - 可用栈区为 `[buf + 32, buf + 32 + stack_size)`，全部填充 `'#'` 以兼容原有的栈水位统计 `task_stack_max_used()`。
- **上下文切换热路径**：
  - 在 `sch_start()` 和 `switch_task()` 切换到 `to_task` 时调用 `hal_vm_switch(&to_task->vm_space)`；
  - 直接将预编码的 `guard_rbar/rasr` 和 `slots[0..1].rbar/rasr` 写入 MPU 寄存器并执行 `dsb` / `isb`。

### 4.3 `MemManage_Handler` 故障精确诊断
- 在 `arch/arm/mm/mpu.c` 中启用 `SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk`；
- 当触发 `MemManage_Handler` 时，提取异常栈帧（MSP 或 PSP）及 `SCB->CFSR`、`SCB->MMFAR`：
  1. 若 `MMFAR` 位于 `[current->stack_guard_base, current->stack_guard_base + 32)` 或触发 `MSTKERR`：打印 **`[MPU FAULT] Task '<name>' (pid=%u) STACK OVERFLOW at 0x%08lx`**；
  2. 若 `MMFAR < 0x40`：打印 **`[MPU FAULT] Task '<name>' NULL pointer dereference at 0x%08lx`**；
  3. 否则打印详细的读写/取指越界地址与寄存器现场，随后调用故障处理钩子（支持测试钩子捕获或安全终止违规任务）。

---

## 5. 阶段 3：MMU 虚拟内存子系统设计 (`kernel/mm/vmm_mmu.c` + 多架构页表)

### 5.1 多级页表引擎 (`hal_vm_*`)
在 `arch/arm64/hal/hal_arm64.c`、`arch/riscv/hal/hal_riscv.c`、`arch/x86/hal/hal_x86_64.c` 及通用页表辅助层实现真实的多级页表管理：
- **页表分配**：通过 `alloc_page(GFP_KERNEL | GFP_ZERO)` 动态分配 4KB 页表页；
- **`hal_vm_map(space, vaddr, paddr, size, prot_flags)`**：
  - 校验 `vaddr`、`paddr`、`size` 4KB 页对齐；
  - 逐页 Walk 多级页表（缺失中间级时自动分配页表页），将 `HAL_VM_PROT_*` 转换为架构硬件 PTE 位（ARM64 4K L0~L3 描述符、RISC-V Sv39 R/W/X/U/V/A/D 位、x86_64 PML4 P/RW/US/NX 位）；
- **`hal_vm_unmap(space, vaddr, size)`**：
  - 清除对应 PTE 并刷 TLB；若某级页表页下属条目全部清空，自动释放该页表页回 Buddy；
- **`hal_vm_translate(space, vaddr, &out_paddr, &out_prot)`**：
  - 软件遍历页表，返回给定虚拟地址映射的物理地址与权限标志（未映射返回 `-ENOENT`）；
- **`hal_vm_space_destroy(space)`**：
  - 深度优先遍历并释放所有非共享页表页，确保进程退出时 0 页表泄漏。

### 5.2 VMA 虚拟内存区域与缺页处理 (`kernel/mm/vmm_mmu.c`)
```c
#define NOS_VM_FLAG_ANON        (1U << 0) /* 匿名内存映射 */
#define NOS_VM_FLAG_DEMAND      (1U << 1) /* 按需分页（访问时才分配物理页） */
#define NOS_VM_FLAG_COW         (1U << 2) /* 写时复制（Copy-on-Write） */
#define NOS_VM_FLAG_FIXED       (1U << 3) /* 固定虚拟地址映射 */
#define NOS_VM_FLAG_OWN_PAGES   (1U << 4) /* unmap 时自动释放底层物理页 */

struct nos_vm_area {
    uintptr_t          vm_start;
    uintptr_t          vm_end;
    uint32_t           vm_prot;
    uint32_t           vm_flags;
    struct list_head   list;
};
```
- **核心接口**：
  - `int nos_vm_mmap(struct nos_vm_space *space, uintptr_t vaddr_hint, size_t size, uintptr_t paddr, uint32_t prot, uint32_t flags, uintptr_t *out_vaddr);`
  - `int nos_vm_munmap(struct nos_vm_space *space, uintptr_t vaddr, size_t size);`
  - `int nos_vm_mprotect(struct nos_vm_space *space, uintptr_t vaddr, size_t size, uint32_t new_prot);`
  - `struct nos_vm_area *nos_vm_find_vma(struct nos_vm_space *space, uintptr_t vaddr);`
  - `int nos_vm_clone(struct nos_vm_space *dst, struct nos_vm_space *src);`（将可写匿名 VMA 标记为 `NOS_VM_FLAG_COW`，递增物理页 `use_cnt`，并将双方 PTE 降级为只读）
  - `int nos_vm_handle_fault(struct nos_vm_space *space, uintptr_t fault_vaddr, uint32_t access_prot);`
    - **按需分页路径**：当 `fault_vaddr` 命中 `NOS_VM_FLAG_DEMAND` VMA 且尚未映射物理页时，自动调用 `alloc_page(GFP_KERNEL | GFP_ZERO)` 并 `hal_vm_map`；
    - **写时复制路径**：当写入命中 `NOS_VM_FLAG_COW` VMA 且当前 PTE 为只读时，检查物理页 `page->use_cnt`：若 `> 1`，分配新页、拷贝 4KB 数据、递减旧页引用计数并重映射为可写；若 `== 1`，直接将原页重映射为可写并清除 CoW 标志。

---

## 6. Kconfig 配置项设计 (`Kconfig`)

在 `menu "Memory Management"` 下新增/完善如下可配置项：
- `CONFIG_MM_TLSF` (`bool "Enable TLSF O(1) low-fragmentation allocator"`, 默认在 `ARCH_ARM && !MMU` 下为 `y`)
- `CONFIG_MM_SLUB` (`bool "Enable SLUB kmem_cache small-object allocator"`, 默认在 `MMU` 或大内存板级下为 `y`，也可与 Buddy 共存开启)
- `CONFIG_MPU` (`bool "Enable MPU / PMP memory protection"`, `depends on ARCH_HAS_MPU`, 在 Cortex-M3/M4 板级可开启)
- `CONFIG_MPU_STACK_GUARD` (`bool "Enable 32-byte hardware MPU task stack guard"`, `depends on MPU`, `default y`)

---

## 7. 验证与测试标准

1. **主机端回归测试套件 (`tests/run_host_regressions.py`)**：
   - `run_mm_buddy_test.py`：验证改进型标准 Buddy 的首页链表、跨阶分裂合并与 `alloc_pages_exact`。
   - `run_mm_block_test.py`：验证现有 `mm_block` / `kmalloc` 接口与并发安全回归。
   - `run_mm_tlsf_test.py`（新增）：验证 TLSF $O(1)$ 分配释放、相邻物理块 100% 合并、`kmalloc_aligned`（32B/4096B）、`kfree_by_pid`、多线程并发及低碎片率指标。
   - `run_mm_slub_test.py`（新增）：验证 `kmem_cache` 生命周期、Slab 页复用与释放、大块直通 Buddy。
   - `run_mm_mpu_test.py`（新增）：验证 PMSAv7 `RBAR`/`RASR` 计算、SRD 子区域掩码、32B 栈底护栏配置及 `MemManage` 故障分类诊断。
   - `run_mm_vmm_test.py`（新增）：验证多级页表 Walk (`map`/`unmap`/`translate`/`destroy` 零泄漏)、VMA 合并与拆分、Demand Paging 按需分配及 CoW 写时复制。
2. **全板级编译与 QEMU 冒烟测试**：
   - `nk60-v2`、`qemu-stm32`、`wl-001`、`zj-v3` 四大预设 100% 编译通过，零新增警告；
   - `python3 tests/qemu_stm32_smoke.py build/qemu-stm32/nos.elf` 冒烟测试通过，且 `free` 命令显示的可用内存较重构前显著提升。
