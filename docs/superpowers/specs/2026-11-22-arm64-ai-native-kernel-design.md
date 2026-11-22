# NOS ARM64 AI-Native 混合内核架构设计与演进规范

- **日期**：2026-11-22
- **状态**：已评审确认（方案 A：Capability-Native 混合内核 + AI 一等内核对象）
- **适用平台**：
  - 参考验证平台：QEMU `virt`（ARMv8-A Cortex-A53 / GICv3 & GICv2 自动兼容）
  - 目标演进平台：ARMv8.2+ / ARMv9 端侧与边缘 AI SoC（如 RK3588 big.LITTLE + NPU、SVE/SME、SMMUv3、PAC/BTI/MTE）
- **关键范围**：
  - Stage 0~1：构建系统与 freestanding libc 适配、64 位 MM/VMM 解耦修复、ARM64 核心底座（EL2→EL1、Higher-Half MMU、异常向量、GICv3/v2、Generic Timer、Per-CPU、上下文切换、CPU 特性探测、PSCI）及 `qemu-virt-arm64` 启动到 `nShell` 闭环（本轮落地代码）；
  - Stage 2~4：PSCI SMP、EL0 用户态与 `SVC` 系统调用、CDT 可撤销能力树、`Agent Cell`、`VMO/TensorObj` 与 OS 级 KV-Cache 分页、`RingIPC` 强类型异步工具环、`sched_infer` + `AccelQueue`、`SMMUv3` 与 ARMv9 安全硬化（完整架构规范与 UAPI/ABI 契约）。

---

## 1. 背景、定位与核心架构原则

### 1.1 为什么 AI 时代需要新的底层内核而不是“再造一个 Linux”

传统通用内核（以 Linux 为代表）建立在 1970 年代的分时系统抽象之上，在面对大模型推理（LLM Inference）与自主智能体（Autonomous Agent）负载时存在四重结构性错配：

1. **进程/容器模型对 Agent 过重且信任模型颠倒**：
   - Agent 的执行特征是**高频派生、短生命周期、带推测/试错回滚、且内部运行不可信外部内容（Prompt Injection 风险）**。
   - Linux 进程拥有隐式的环境继承与 UID 权限，需叠加 `UID + Namespace + cgroup + seccomp-bpf + LSM` 五层补丁才能构造一个 Agent 沙箱，且毫秒级快照回滚依赖重型 `fork()+CRIU` 或外部文件系统。
2. **均质 4KB 匿名页与用户态模拟无法匹配张量与 KV-Cache 内存语义**：
   - 大模型推理的核心负载是**只读巨型权重（GB 级）**、**动态增长且存在公共前缀的 KV-Cache（PagedAttention）**以及**CPU/NPU 跨器件零拷贝张量传递**。
   - 用户态框架只能在大块匿名内存上自行实现软件页表和引用计数，既浪费地址空间局部性与 TLB，也无法在多个并发 Agent 之间由内核保证跨沙箱安全共享与 OOM 前的协作驱逐。
3. **基于逐次 Syscall 字节流的 IPC 无法胜任高频强类型工具调用**：
   - Agent 需要以毫秒级并发扇出数十个工具调用（文件、网络、MCP 服务、数据库、加速器作业）并流式消费 Token，传统 `write/read` + 序列化拷贝带来的陷入开销与能力校验盲区成为主要瓶颈。
4. **CFS/EEVDF 调度器缺乏推理 SLO 与异构矢量感知**：
   - 通用公平调度无法理解大模型推理中 **Prefill（算力/SVE 密集）** 与 **Decode（访存带宽/低尾延迟 TPOT 敏感）** 的根本矛盾，也没有原生的端到端截止期继承与多核 Gang 协同机制。

### 1.2 NOS ARM64 顶层设计哲学：Capability-Native 混合内核

NOS 在 ARM64 上不走宏内核老路，也不做纯微内核在加速器与页表热路径上的高频 IPC 折返，而是采用 **Capability-Native 混合内核（seL4/Zircon 基因 × AI Native 原语）**：

- **与 Cortex-M 档保持统一内核对象模型**：复用并演进 NOS 现有的 Generation-Counted Capability Table（`kernel/cap/`）、`nos_ipc_msg`（64B 同步快速路径）、`sched_class`（`kernel/sched/`）与 DeviceTree 设备模型。
- **混合内核特权边界（性能与隔离兼得）**：
  - **EL1 数据面直通**：CDT 能力仲裁、Cell 预算与审计、`VMO/KV-Cache` 页表管理、`RingIPC` 无锁环数据面、`sched_infer` 调度、GICv3、SMMUv3/IOMMU 与核心加速器（NPU/VirtIO-Accel）命令队列留在 **EL1**。
  - **EL0 策略与不可信隔离**：VFS/文件系统服务、网络协议栈、MCP 工具网关、模型调度器、POSIX/musl 兼容层以及所有不可信的 **Agent Cell** 沙箱运行在 **EL0**，零默认权限，仅通过显式授予的 Capability 句柄访问资源。

---

## 2. ARM64 硬件底座与混合内核分层设计

### 2.1 特权级职责分层（EL2 / EL1 / EL0）

```text
┌────────────────────────────────────────────────────────────────────────────┐
│ EL0（非特权隔离态：每 Agent Cell 独立 TTBR0_EL1 + 16-bit ASID + CNode）     │
│  ├─ 系统服务层：VFS 服务、网络/MCP 工具网关、Model Registry、nShell、      │
│  │              musl POSIX 兼容层、Agent 编排器                            │
│  └─ 业务负载层：独立 Agent Cell 沙箱（零默认权限、硬预算、CoW 快照/回滚）  │
╞══════════════════════ SVC #0 / RingIPC 共享 VMO 零拷贝边界 ════════════════╡
│ EL1（混合内核本体：共享 TTBR1_EL1 Higher-Half，W^X + PAN 硬件强制隔离）    │
│  ├─ 机制微核层：CDT 能力树、Cell 预算/审计/快照、VMO/KV-Pager、            │
│  │              RingIPC + 64B 同步快速路径、sched(RT/Infer/Fair/IDLE)      │
│  ├─ 性能直通层：AccelCore(NPU/GPU 队列抽象)、SMMUv3/SVA 驱动、             │
│  │              VirtIO-Accel/Blk、PL011 Early/Panic 串口                   │
│  └─ ARM64 底座：EL2→EL1 启动、异常向量、MMU/ASID/TLBI、GICv3(+GICv2 回退)、│
│                 Generic Timer、PSCI SMP、Per-CPU、惰性 FP/SVE/SME、特性表  │
╞══════════════════════ HVC / SMC 固件与机密扩展边界 ════════════════════════╡
│ EL2 / EL3（Firmware/TF-A）：启动时由 EL2 降级至 EL1；Stage 4 预留 CCA Realm │
└────────────────────────────────────────────────────────────────────────────┘
```

> **Stage 1 务实过渡策略**：Stage 1 首轮启动时复用内建 VFS/RamFS/nShell 在 EL1 跑通串口中断交互闭环，但虚拟内存布局（Higher-Half）、异常向量、288B `pt_regs` 与 `SVC #0` 派发入口从第一天即按完整 EL1/EL0 架构构建，Stage 2 开启 EL0 用户态时无需推倒重来。

---

### 2.2 Higher-Half 双页表虚拟地址布局

默认采用 `CONFIG_ARM64_VA_BITS=39`（3 级 4KB 页表，512GB Lower + 512GB Higher，减少页表层级与 TLB Miss 遍历开销），同时支持 Kconfig 切换至 `48` 位（4 级页表）。

#### 2.2.1 内核空间 `TTBR1_EL1`（39-bit 下 `0xFFFFFF80_00000000` ~ `0xFFFFFFFF_FFFFFFFF`）

| 虚拟地址区间 | 跨度 | 页表粒度与属性 | 职责说明 |
| --- | --- | --- | --- |
| `0xFFFFFF80_00000000` ~ `0xFFFFFFBF_FFFFFFFF` | 256 GB | 1GB / 2MB Block，`MT_NORMAL`，`RW + PXN + UXN` | **Linear Physmap**：物理 RAM 线性直映（`va = PHYSMAP_BASE + pa`），内核页表分配与物理/虚拟转换 `O(1)` 零查表开销 |
| `0xFFFFFFC0_00000000` ~ `0xFFFFFFCF_FFFFFFFF` | 64 GB | 2MB / 4KB，`MT_DEVICE_nGnRnE` 或 `nGnRE`，`RW + PXN + UXN` | **MMIO / ioremap / Fixmap 窗口**：GICv2/v3、PL011/DW-UART、SMMUv3、NPU/VirtIO 寄存器重映射 |
| `0xFFFFFFD0_00000000` ~ `0xFFFFFFEF_FFFFFFFF` | 128 GB | 4KB / 64KB `PTE_CONT`，`RW + PXN + UXN` | **KVMalloc / 带 Guard Page 内核栈 / Per-CPU / Non-Cacheable DMA 池** |
| `0xFFFFFFF0_00000000` ~ `0xFFFFFFFF_FFFFFFFF` | 64 GB | 4KB 严格 **W^X**（`.text` `RO+EXEC+UXN`；`.rodata` `RO+PXN+UXN`；`.data/.bss` `RW+PXN+UXN`） | **内核镜像区**：固定高半核 VMA，彻底解耦底层 SoC 物理加载基址（QEMU `0x40200000` 与 RK3588 `0x00200000` 共享同一高半核布局） |

#### 2.2.2 用户态 / Agent Cell 空间 `TTBR0_EL1`（39-bit 下 `0x0000_0000_0000_0000` ~ `0x0000_007F_FFFF_FFFF`）

| 虚拟地址区间 | 跨度 | 面向 AI 负载的设计语义 |
| --- | --- | --- |
| `0x0000_0000_0000_0000` ~ `0x0000_0000_0000_FFFF` | 64 KB | 永不映射的 Null-Guard 陷阱区 |
| `0x0000_0000_0001_0000` ~ `0x0000_003F_FFFF_FFFF` | 256 GB | 常规 ELF 代码/数据/堆/栈/匿名映射（强制 W^X） |
| `0x0000_0040_0000_0000` ~ `0x0000_005F_FFFF_FFFF` | 128 GB | **Tensor / Model VMO 窗口**：优先 2MB Block / 64KB `PTE_CONT`，权重只读跨 Cell 共享，与 SMMUv3 SVA 共享 VA 语义 |
| `0x0000_0060_0000_0000` ~ `0x0000_006F_FFFF_FFFF` | 64 GB | **KV-Cache 托管窗口**：虚拟连续张量、物理离散 64KB KV 块按需分配与前缀 CoW 共享 |
| `0x0000_0070_0000_0000` ~ `0x0000_007F_FFFF_FFFF` | 64 GB | **RingIPC / vDSO / Cell 只读遥测页窗口**：零拷贝 SQ/CQ 环 + 无需陷入 EL1 即可读取的高精度时钟与 Cell 实时预算 |

#### 2.2.3 现有 `vmm_mmu.c` 缺陷与解耦策略

当前 [kernel/mm/vmm_mmu.c](file:///Volumes/nick-disk/projects/nos/kernel/mm/vmm_mmu.c) 在页表中写入了软件自定义位（如 `NOS_PTE_READ = 1<<2`）且假定 `vaddr == paddr`。直接将其写入 ARM64 `TTBR0_EL1` 会因缺少 `AF=1`（Access Flag）、缺少标准 ARMv8 `PTE_TYPE_PAGE (0b11)` 与 `MAIR` 索引而在开启 MMU 后立即引发异常。

因此采用**便携接口 + 硬件后端分离**：
- 保留 `kernel/mm/vmm_mmu.c` 作为软件 VMA 区间管理、CoW 引用计数及主机逻辑单测层（确保 `tests/host/run_mm_vmm_test.py` 100% 持续通过）；
- 在 `arch/arm64/mm/mmu.c` 中实现真正的 **ARMv8-A Stage-1 硬件页表构建器**（`arm64_pte_make_leaf`、`arm64_pt_map_range`、`arm64_pt_unmap_range`、`arm64_pt_mprotect`、`arm64_pt_translate`、`arm64_asid_alloc`、`arm64_ioremap`），严格遵循 Break-Before-Make（BBM）先清零旧项、广播 `tlbi vaae1is` + `dsb ish` + `isb` 后再写新描述符，并强制拒绝任何同时具备 `WRITE` 与 `EXEC` 的映射（返回 `-EINVAL`）。

---

### 2.3 双段启动、EL2→EL1 降级与位置无关开启 MMU

1. **物理加载地址防 DTB 覆盖**：
   - QEMU `virt` 默认将 DTB 置于 DRAM 基址 `0x40000000` 并通过 `x0` 传参。
   - 内核物理装载入口设为 `0x40200000`（DRAM 偏移 +2MB），保留 `[0x40000000, 0x40200000)` 完整存放 DTB，启动阶段将其拷入或映射访问，绝不被内核镜像踩踏。
2. **双段链接契约（`board/qemu-virt/nos_kernel.ld.S`）**：
   - `.boot` 段（`.text.boot`、`.rodata.boot`、`.bss.boot`）：`VMA == LMA = 0x40200000`；
   - 高半核主镜像段（`.text`、`.rodata`、`.data`、`.bss`）：64KB 对齐后 `VMA = 0xFFFFFFF0_00000000 + offset`，`LMA` 紧随 `.boot` 物理地址之后——确保生成的 `nos.bin` 体积紧凑且无任何虚拟地址空间空洞；同时各段之间按 4KB 页对齐边界切分，使启动后可直接逐页施加 W^X 权限。
3. **启动汇编精确时序（`arch/arm64/boot/start.S`）**：
   - 屏蔽 DAIF 中断，保存 `x0`（DTB PA）与初始 `CurrentEL`。
   - 从核（`MPIDR_EL1 & 0xFFFFFF != 0`）进入 `secondary_park` 等待后续 PSCI 唤醒。
   - 若在 EL2：初始化 `HCR_EL2 = (1 << 31)`（EL1 为 AArch64）、`CNTHCTL_EL2 |= 3`（允许 EL1 读物理计数器）、`CNTVOFF_EL2 = 0`、`CPTR_EL2 = 0`；读取 `ID_AA64PFR0_EL1.GIC`，若支持 GICv3 SysReg 则置位 `ICC_SRE_EL2 = 0xF`；设置 `SPSR_EL2 = 0x3c5`（EL1h + DAIF 全屏蔽）与 `ELR_EL2 = el1_entry`，执行 `eret` 落入 EL1。
   - 在 EL1 物理地址下按 LMA 清零高半核 `.bss` 与 `.bss.boot`。
   - 填充静态引导页表：
     - `TTBR0_EL1`：临时 2MB Block 恒等映射（覆盖 `0x40000000..0x42000000`），保障开 MMU 指令平滑流转及早期 DTB 读取；
     - `TTBR1_EL1`：同时建立 Higher-Half Physmap（`0xFFFFFF80_40000000+` 512MB）、MMIO 窗（`0xFFFFFFC0_08000000+` 64MB）与 Kernel Image（`0xFFFFFFF0_00000000+` 32MB）。
   - 配置 `MAIR_EL1`（Attr0=Normal WBWA `0xFF`, Attr1=Device-nGnRnE `0x00`, Attr2=Device-nGnRE `0x04`, Attr3=Normal-NC `0x44`, Attr4=Normal-WT `0xBB`）与 `TCR_EL1`（依据 `CONFIG_ARM64_VA_BITS` 设置 `T0SZ`/`T1SZ`，动态读 `ID_AA64MMFR0_EL1.PARange` 填 `IPS`，启用 16-bit ASID），使能 `SCTLR_EL1`（`M | C | I | SA | SA0`）。
   - 切换 `SP` 至高半核初始内核栈，绝对间接跳转至 `nos_arm64_boot(dtb_pa, boot_el)`。
   - `nos_arm64_boot` 内依次完成：安装 `VBAR_EL1` → `arm64_percpu_init(0)` → `arm64_cpufeature_init()` → `arm64_mmu_init_kernel_wx()`（用 4KB 页表细化内核区 W^X 并**撤销临时 `TTBR0_EL1`**）→ 跳转平台无关内核主入口 `nos_start()`。

---

### 2.4 运行时 CPU 特性探测（`arch/arm64/cpu/cpufeature.c`）

启动时读取 ARMv8 ID 寄存器（`ID_AA64ISAR0/1_EL1`、`ID_AA64PFR0/1_EL1`、`ID_AA64MMFR0/1/2_EL1`、`CTR_EL0`），构造全局只读能力描述 `struct arm64_cpu_caps`：

| 类别 | 探测特性 | 内核用途 |
| --- | --- | --- |
| **并发与内存** | `LSE` (v8.1 Atomics), `AFDBM` (硬件 Access/Dirty), `ASID16`, `PARange` | LSE 原子锁、KV-Cache 热页 `AF` 位零开销追踪、65535 空间隔离、48-bit 物理寻址 |
| **AI 算力向量** | `ASIMD/NEON`, `FP16`, `DotProd`, `I8MM`, `BF16`, `SVE/SVE2` (`VL`), `SME` | 供 `sched_infer` 识别异构大核推理算力画像，按需分配/切换大向量寄存器现场 |
| **安全隔离** | `PAN`, `PXN/UXN`, `BTI`, `PAC (PAuth)`, `MTE`, `RME` | 阻止内核误解引用 EL0 指针（PAN）、控制流防篡改（PAC/BTI）、Cell 内存标签防越界（MTE） |
| **平台中断** | `GIC` (SysReg v3/v4 接口) | 自动决定 GICv3 系统寄存器快速路径或 GICv2 MMIO 兼容路径 |

---

### 2.5 异常向量、GICv3/v2 中断子系统与无漂移 Generic Timer

1. **异常向量与精确 Fault 诊断（`vectors.S` + `trap.c`）**：
   - `.section .vectors` 严格 2KB 对齐（`.align 11`），涵盖 4 组 × 4 异常类型共 16 条向量槽。
   - 统一保存 288 字节（16B 对齐）`struct arm64_pt_regs`（`x0..x30`, `sp`, `pc(elr_el1)`, `pstate(spsr_el1)`, `esr`, `far`）。
   - `ESR_EL1.EC` 精准路由：`EC_SVC64` 派发 `nos_syscall_dispatch(x8, x0..x5)`；`EC_FP_ASIMD`/`EC_SVE`/`EC_SME` 路由至惰性向量上下文恢复；`EC_DABT_LOW`/`EC_IABT_LOW` 路由至 VMM/VMO 缺页裁决；内核异常则解码 `EC` / `DFSC` / `FAR_EL1` 并沿 `FP(x29)/LR(x30)` 结合 `kallsyms` 输出完整符号栈回溯。
2. **中断控制器 GICv3 + GICv2 自动兼容（`arch/arm64/kernel/irq.c`）**：
   - 通过 `arm64_gic_init(cfg)` 运行时探测 `ID_AA64PFR0_EL1.GIC` 与 `GICR` 配置：
     - **GICv3 主路径**：开启 `ICC_SRE_EL1.SRE=1`，配置 `GICD_CTLR = ARE_NS | EnableGrp1NS`，唤醒 Per-CPU `GICR_WAKER`（清 `ProcessorSleep` 并等待 `ChildrenAsleep == 0`），开启 SGI/PPI 分组与 `ICC_PMR_EL1 = 0xFF`、`ICC_IGRPEN1_EL1 = 1`；通过 `ICC_IAR1_EL1 / ICC_EOIR1_EL1 / ICC_SGI1R_EL1` 完成低延迟响应与 IPI（Stage 4 扩展 ITS LPI）。
     - **GICv2 兼容路径**：当硬件无 GICv3 SysReg 或未提供 GICR 时平滑回退至 `GICD + GICC` MMIO，使 QEMU 默认 `-M virt` 与 `-M virt,gic-version=3` 均可零改配置启动。
   - 维护 1020 槽动态 IRQ 分发表 `nos_irq_register(intid, handler, ctx)` / `nos_irq_unregister(intid)`，彻底脱离 MCU 静态向量号绑定模式。
3. **无漂移 ARM Generic Timer（`arch/arm64/kernel/timer.c`）**：
   - 使用 `__uint128_t` 运算（`arm64_timer_ticks_to_ns`）避免 `pct * 10^9` 在 64 位乘法下溢出。
   - 使用 `CNTP_CVAL_EL0` 绝对比较寄存器累加 `tick_interval` 驱动固定频率系统心跳（PPI 30），消除 `CNTP_TVAL_EL0` 累积误差；同时提供 `hal_timer_program_oneshot_ns()` 服务于 `sched_infer` 微秒级 SLO 抢占。

---

### 2.6 Per-CPU、同步上下文切换与锁交接不变量、惰性向量与 PSCI SMP

1. **Per-CPU (`TPIDR_EL1`)**：每核绑定 `struct arm64_percpu`（`cpu_id`, `mpidr`, `irq_nest`, `irq_stack_top`, `fpu_owner`, `cpu_capacity`, `deferred_switch`, `deferred_from`, `deferred_to`），`hal_cpu_id()` 单条 `mrs` 指令 O(1) 返回。
2. **上下文切换与 `rq->lock` 数学闭环**：
   - 内核 `kernel/sched/sched.c` 的 `switch_task()` 在持有 `this_rq()->lock`（并使 `scheduler_lock_nest == 1`、`__nos_spin_irq_depth == 1`、`DAIF.I == 1`）时调用 `context_switch(from, to)`，而在 `sch_heartbeat()`（位于 ISR `irq_entry()`/`irq_exit()` 内部）中也会直接调用 `switch_task()`。
   - 若直接在 ISR 内同步切换，会导致新任务在 `interrupt_nest == 1` 下运行并错乱锁嵌套；为此 ARM64 实现与 Cortex-M PendSV 语义严格等价且无竞态的**双路径统一协议**：
     - 当 `hal_in_irq()` 为真（即位于 ISR 内部）：`context_switch` 仅记录 `percpu->deferred_switch = true`（仅首次置位时保存 `deferred_from = from`，始终更新 `deferred_to = to`）并立即返回，让 `switch_task()` 正常解锁、`irq_exit()` 将 `interrupt_nest` 准确归零；
     - 在异常出口 `arm64_irq_exit_resched()`（此时 `interrupt_nest == 0`、`pt_regs` 位于被中断任务栈顶）：若 `deferred_switch` 为真，加锁 `spin_lock_irq(&this_rq()->lock)` 后调用 `__arm64_context_switch(deferred_from, deferred_to)`，返回后 `spin_unlock_irq(&this_rq()->lock)`；若 `!deferred_switch` 但 `need_resched && scheduler_lock_nest == 0`，直接调用 `switch_task()`；
     - **核心不变量**：`__arm64_context_switch` 的 `arm64_switch_frame` 除保存 `x19..x30` 外，同时在当前任务的内核栈帧上保存/恢复该任务的 `__nos_spin_irq_flags[cpu]`！新创建的任务首次通过 `arm64_task_trampoline` -> `arm64_finish_first_switch()` 进入时，若 `rq->lock` 被持有则调用 `spin_unlock_irq(&rq->lock)` 完成解锁与嵌套计数清零，并显式打开全局中断后进入 `entry(parameter)`。由此保证无论是主动让出、IRQ 抢占切出、还是首次启动的任务，锁计数与 `DAIF` 状态永远严丝合缝。
3. **惰性 FP/NEON/SVE/SME 状态**：
   - ARM64 内核编译采用 `-mgeneral-regs-only`，EL1 绝不触碰向量寄存器。
   - 默认屏蔽 `CPACR_EL1.FPEN`，任务首次使用 SIMD/浮点触发 `EC_FP_ASIMD` 陷阱时才在 `arm64_fpu_trap_handler()` 中打开权限并按 `percpu->fpu_owner` 执行惰性保存/恢复，让 Agent 控制环/IO 代理线程享受零向量切换开销。
4. **PSCI 0.2/1.0 与双轨平台边界**：
   - 统一通过 `HVC` / `SMC` 调用 `PSCI_CPU_ON`、`PSCI_SYSTEM_RESET`、`PSCI_SYSTEM_OFF`。
   - `arch/arm64/` **严禁**硬编码 QEMU 或任何具体板级物理地址；所有地址与设备拓扑通过 `board/<board>/`、`soc/<soc>/` 与 DTB 传入，保证未来接入 RK3588（GIC-600 + DW-8250 + RKNPU + Cortex-A76/A55）时 `arch/arm64/` 零改动复用。

---

## 3. AI 时代四大原生内核核心组件与 UAPI/ABI 契约

面向 AI/Agent 时代的内核不能只是把四个功能硬拼在一起，必须以**一条清晰的资源与执行主线**贯通：

> 执行隔离的载体是 **`Agent Cell`**，内存与模型资产的载体是 **`VMO / TensorObj`**，工具调用与流式通信的载体是 **`RingIPC`**，算力分配与延迟保障的载体是 **`sched_infer + AccelQueue`**；四者统一仲裁于可级联撤销的 **CDT 能力树**。

### 3.0 统一安全基石：双层 CNode 与 CDT 可撤销能力树

基于现有 [kernel/cap/cap_table.c](file:///Volumes/nick-disk/projects/nos/kernel/cap/cap_table.c)（32-bit `index|gen` 句柄、rights 位图、不可伪造 IPC `badge`），在 ARM64 档做如下增强（MCU 档继续保持 16 槽紧凑静态表）：

1. **双层基数 CNode**：每个 Cell 的能力根支持 2 级页式扩展（单 Cell 上限 4096 个能力槽），按需分配释放，消除固定 16 槽上限；32-bit `nos_handle_t` ABI 保持跨架构兼容。
2. **CDT（Capability Derivation Tree）侵入式链表**：每个活跃 `nos_cap_slot` 增加 `parent / first_child / next_sibling / prev_sibling`（与可选 `schema_mask` 约束）。通过 `nos_cap_derive` 派生子能力时保证权限严格单调递减（`new_rights ⊆ src_rights`）；调用 `SYS_NOS_CAP_REVOKE_TREE` 时，内核按 DFS/BFS 原子吊销该节点及其全部子 Agent 派生子树，并同步递增 `gen` 使旧句柄永久失效。
3. **扩展一等内核对象枚举**（[include/uapi/nos/cap.h](file:///Volumes/nick-disk/projects/nos/include/uapi/nos/cap.h)）：
   - 原有：`NOS_CAP_TASK`, `NOS_CAP_VM_SPACE`, `NOS_CAP_ENDPOINT`, `NOS_CAP_NOTIFICATION`, `NOS_CAP_MUTEX`, `NOS_CAP_IRQ`, `NOS_CAP_PHYSMEM`, `NOS_CAP_IOPORT`
   - 新增：`NOS_CAP_CELL`（Agent 沙箱）、`NOS_CAP_VMO`（张量/权重/KV 内存对象）、`NOS_CAP_RING`（异步工具调用环）、`NOS_CAP_ACCEL`（异构加速器队列）、`NOS_CAP_SNAPSHOT`（不可变 Cell 检查点快照）。
   - 新增权限位：`NOS_CAP_RIGHT_AUDIT (1<<7)`、`NOS_CAP_RIGHT_MAP (1<<8)`。

---

### 3.1 组件一：`Agent Cell`（能力沙箱、多维硬预算、强制审计与亚毫秒快照/回滚）

```text
  Orchestrator / Supervisor (持有 NOS_CAP_CELL: RIGHT_MANAGE | RIGHT_AUDIT)
      │
      ▼
┌───────────────────────────── struct nos_cell ─────────────────────────────┐
│ 1. 隔离边界  │ 独立 CNode 根（零默认权限）+ 独立 TTBR0_EL1 + 16-bit ASID │
│ 2. 五维预算  │ cpu_ns / mem_bytes / tool_calls / accel_ops / child_cells  │
│ 3. 遥测窗口  │ 只读映射 4KB Telemetry Page 到 EL0，Agent 零 syscall 读配额│
│ 4. 审计日志  │ 64B 定长环形审计区，EL1 在能力变更/工具调用/违例时强制写入 │
│ 5. 推测分支  │ O(1) CoW Snapshot / Fork / Rollback（目标回滚 < 200 µs）   │
└───────────────────────────────────────────────────────────────────────────┘
```

- **多维硬预算裁决点（内核级不可绕过）**：
  - `cpu_ns_budget`：在 `sched_infer` 与周期时钟滴答中原子扣减；归零即挂起 Cell 内所有线程，并向监管者的 `Notification` 投递 `CELL_EVT_BUDGET_CPU`。
  - `mem_bytes_budget`：在 `SYS_NOS_VM_MAP`、匿名缺页与 `VMO_KIND_KVCACHE` 扩容时原子校验；超限返回 `-EDQUOT`（或 `-ENOMEM`）而不是挤占全局物理内存。
  - `tool_calls_budget` / `accel_ops_budget`：在 `SYS_NOS_RING_ENTER` 提交与 `SYS_NOS_ACCEL_SUBMIT` 敲门铃时原子扣减，在内核门禁直接阻断 Prompt Injection 引起的无限工具循环或算力攻击。
  - `max_child_cells`：防止递归派生子 Agent 炸弹。
- **Tree-of-Thought 推测执行原语**：
  - **Snapshot (`NOS_CELL_OP_SNAPSHOT`)**：遍历 Cell 内可写 VMO 页表项，将其临时改为只读并置软件位 `PTE_SW_COW`、递增页引用计数，同时冻结 CNode 状态与 KV-Cache 块树（**零数据拷贝**），产出不可变 `NOS_CAP_SNAPSHOT`。
  - **Rollback (`NOS_CELL_OP_ROLLBACK`)**：丢弃分支期间分配的 CoW 脏页与增量 KV 块，恢复快照态的页表映射与预算剩余，实现亚毫秒级回退到检查点。
  - **Fork (`NOS_CELL_OP_FORK`)**：派生子 Cell 并从父预算划拨配额（严禁子级配额超出父级剩余），共享只读模型权重与 KV 前缀。

---

### 3.2 组件二：`VMO / TensorObj` + OS 级 KV-Cache 分页与 SMMUv3 统一地址空间

通过统一内核内存对象 `struct nos_vmo` 替代散落的匿名内存、文件 mmap 与驱动私有 DMA 池：

| VMO 类型 (`enum nos_vmo_kind`) | 物理组织与 ARM64 硬件映射 | AI 时代核心价值 |
| --- | --- | --- |
| `NOS_VMO_ANON` | 4KB/2MB 按需分页 + CoW | Agent 常规堆、栈与临时工作内存 |
| `NOS_VMO_WEIGHT` | **2MB Block / 64KB `PTE_CONT`** + `NOS_VMO_OP_SEAL` 永久不可变封印 + 哈希指纹 | 多 GB 模型权重跨任意数量 Agent Cell **只读零拷贝映射**，大幅压缩 TLB Miss；封印后内核拒绝任何可写映射或修改 |
| `NOS_VMO_KVCACHE` | 虚拟连续张量地址 + 离散 **64KB KV 块**（16×4KB + `PTE_CONT` 位）按需填充 + Radix 前缀索引 | ① **OS 级 PagedAttention**：推理算子直接顺序访存虚拟地址；② **前缀零拷贝共享**：公共 System Prompt / RAG 知识库 KV 块经 `NOS_VMO_OP_CLONE_PREFIX` 跨 Cell 硬件 CoW 共享；③ **可驱逐协议**：标记为 `RECOMPUTE_DROPPABLE` 的 KV 页结合 ARMv8.1 `AF` 访问位在全局内存紧张时有序回收并向推理引擎发 Notification 触发重算，消除暴力 OOM Kill |
| `NOS_VMO_DMA_TENSOR` | 物理连续块或 Scatter-Gather IOMMU 页表 + 显式缓存属性（`IO_COHERENT` / `CLEAN_INV`） | CPU、NPU、GPU、VirtIO 间零拷贝传递激活张量与大工具载荷 |

- **SMMUv3 统一地址空间（Stage 4 落地，页表属性从 Stage 1 对齐）**：
  - ARM64 Stage-1 页表描述符与 ARM SMMUv3 Stage-1 格式同源。支持 PASID/SVA 的硬件可将 Cell 的 `TTBR0_EL1`（受限 VMO 子集）或专用 IOAS 页表直接绑定到加速器流 ID（StreamID + SubstreamID），使 NPU 硬件直接以虚拟地址访问共享权重与离散 KV 块，并由硬件保证跨 Agent DMA 边界不可越权。

---

### 3.3 组件三：`RingIPC` 异步提交/完成环 + 强类型工具调用信封

与现有 64 字节同步 RPC [struct nos_ipc_msg](file:///Volumes/nick-disk/projects/nos/include/uapi/nos/ipc.h#L32-L40) 形成互补：**短小同步内核控制走 `nos_ipc_msg`，高频 Agent 工具调用、Token 流与加速器任务走 `RingIPC`**。

```text
  Agent Cell (EL0)                              EL1 内核门禁               Tool Server / Accel (EL0/EL1)
   写入 64B SQE ─▶ [ 共享 VMO 无锁 SQ 环 ] ──▶ 校验 target_cap & schema_id ──▶ 投递至服务 CQ / NPU 队列
   (stlr 更新 tail)                                扣减 tool_calls_budget         (无内核数据拷贝！)
                                                   写入 64B 不可伪造审计项
   读取 32B CQE ◀─ [ 共享 VMO 无锁 CQ 环 ] ◀────────────────────────────────── 提交完成状态 & 结果 VMO
```

- **ABI 严格定长契约**（详见 `include/uapi/nos/ai.h`）：
  - `struct nos_ring_sqe`（**64 字节 = 1 Cache Line**）：包含 `opcode`（`TOOL_CALL / STREAM_CHUNK / TOKEN_EMIT / VMO_XFER / ACCEL_JOB / CANCEL`）、`flags`（`ZERO_COPY_RO / MOVE_SLICE / NEEDS_AUDIT / PROPAGATE_DEADLINE`）、`target_cap`、`req_id`、`schema_id`（64-bit 工具接口 schema 哈希，如 MCP Tool/FlatBuffers 契约指纹）、`vmo_cap + vmo_offset + vmo_len`（只传递数据切片能力，不拷贝大 Prompt 或张量）、`deadline_ns`（触发跨服务截止期继承）及 16B `inline_arg[2]`。
  - `struct nos_ring_cqe`（**32 字节**）：包含 `req_id`、`status`（0 或 `-errno`）、`result_vmo_cap`、`result_len`、`elapsed_ns`、`flags`。
  - 批量陷入原语 `SYS_NOS_RING_ENTER` 支持单次系统调用提交 $M$ 个 SQE 并可选等待至少 $N$ 个 CQE（专用核模式下支持用户态无陷入 SQPoll）。

---

### 3.4 组件四：`sched_infer` 推理 SLO 调度器 + EL1 `AccelQueue`

在现有 [include/nos/sched.h](file:///Volumes/nick-disk/projects/nos/include/nos/sched.h) `struct sched_class` 框架下无缝插入 `infer_sched_class`：

1. **调度类优先级链**：`rt_sched_class`（硬实时） > **`infer_sched_class`（AI 推理与 Agent SLO）** > `fair_sched_class`（普通服务） > `idle_sched_class`。
2. **EDF + CBS 尾延迟保障（`kernel/sched/sched_infer.c`）**：
   - 每个任务绑定推理角色（`NOS_INFER_ROLE_DECODE / PREFILL / AGENT_CTRL / TOOL_WORKER`）及 SLO 参数 `(runtime_ns, deadline_ns, period_ns)`。
   - 调度器以 **EDF（最早绝对截止期优先）** 保证 **TTFT**（首字延迟）与 **TPOT**（逐 Token 生成延迟），并通过 **CBS（恒定带宽服务器）** 约束最大带宽配额，防止 Prefill 洪峰或异常死循环饿死系统。
   - **截止期继承（Deadline Inheritance）**：当高紧迫度 Decode 任务在 `RingIPC` 或 `AccelQueue` 上等待某 `TOOL_WORKER` 完成时，将其剩余 `deadline_ns` 临时继承给服务方线程。
3. **异构核感知（EAS）与多核 Gang 协同**：
   - 结合 `arm64_cpu_caps` + DTB `capacity-dmips-mhz`：`PREFILL` 倾向投递至支持 SVE/SME/I8MM 的大核集群；`DECODE` 在访存瓶颈下倾向分散至不同 LLC 簇；`AGENT_CTRL` 收敛至小核以降低端侧功耗。
   - 协同算子可通过 `NOS_SCHED_OP_GANG_JOIN` 组成 `nos_gang_group`，利用 GICv3 SGI 实现同簇多核同步换入，彻底消除并行矩阵乘“部分线程被抢占导致其余核空转自旋”的尾部性能断崖。
4. **EL1 混合内核加速器队列（`kernel/accel/`，`NOS_CAP_ACCEL`）**：
   - 性能关键加速器驱动（Stage 4 落地 QEMU `virtio-accel` 参考设备与 RK3588 `rknpu` 接口）驻留 EL1，实现统一操作集 `struct nos_accel_ops`（`query_caps / bind_vmo / submit_job / cancel_job / reset_ctx`）。
   - EL0 推理引擎无需把权重复拷进内核，只需持有经过能力授权的 `NOS_CAP_ACCEL` 与 `NOS_CAP_VMO` 调用 `SYS_NOS_ACCEL_SUBMIT`，EL1 完成权限/预算/IOMMU 校验后直写硬件 Doorbell，完成后经 `Notification` 或 `RingIPC CQ` 唤醒。

---

### 3.5 UAPI 系统调用契约（严格向后兼容 `0..15`）

| 系统调用号 | 宏名 | 摘要 |
| --- | --- | --- |
| `0..15` | 既有核心原语 | `YIELD / EXIT / DEBUG_CONSOLE / CLOCK_GET / SLEEP_UNTIL / THREAD_* / VM_* / IPC_* / NOTIFY_* / CAP_CONTROL / IRQ_CONTROL`（**完全兼容不动**） |
| `16` | `SYS_NOS_CELL_CONTROL` | 创建/销毁 Cell、配置五维预算、Fork、Snapshot、Rollback、读取 64B 审计记录 |
| `17` | `SYS_NOS_VMO_CREATE` | 创建指定类型（`ANON / WEIGHT / KVCACHE / DMA_TENSOR`）与大页偏好的 `VMO` |
| `18` | `SYS_NOS_VMO_CONTROL` | 不可变封印（Seal）、CoW 克隆、KV 前缀克隆、注册驱逐策略、Cache Clean/Inv |
| `19` | `SYS_NOS_RING_CREATE` | 创建异步 `RingIPC` SQ/CQ 共享环并绑定双向 `Notification` |
| `20` | `SYS_NOS_RING_ENTER` | 批量提交 SQE + 可选收割 CQE 与 `deadline_ns` 传递（Stage 3 热路径） |
| `21` | `SYS_NOS_SCHED_INFER` | 绑定 `infer_sched_class` EDF/CBS 参数、管理 Gang 组、查询 CPU 异构算力表 |
| `22` | `SYS_NOS_ACCEL_SUBMIT` | 校验能力与预算后直通提交 `VMO` 张量计算作业到 EL1 加速器队列 |
| `23` | `SYS_NOS_CAP_REVOKE_TREE` | 基于 CDT 能力树级联撤销子句柄并排空在途请求 |

---

## 4. Stage 0 ~ Stage 4 全阶段演进路线图

| 阶段 | 核心目标与交付物 | 验收标准 |
| --- | --- | --- |
| **Stage 0**（本轮落地） | 工具链 libc 自动探测与隔离式内核 freestanding libc；64 位 `pfn`/`size_t` 截断修复；`arch/arm64/compiler_flags.cmake` 启用 `-mgeneral-regs-only`；AI 原生 UAPI 头文件预置 | 所有既有 Cortex-M 预设（`nk60-v2`/`qemu-stm32`/`wl-001`/`zj-v3`）构建零回归；`run_host_regressions.py`、`test_arch_flags.cmake`、`test_soc_headers.cmake`、`check_layer_deps.py` 全通过 |
| **Stage 1**（本轮落地） | `arch/arm64/` 完整底座（`EL2→EL1` 启动、Higher-Half 39/48-bit 硬件 MMU、2KB 异常向量、GICv3+v2 自动兼容、128-bit 防溢出 Generic Timer、Per-CPU、完整锁交接上下文切换、CPU 特性探测、PSCI）；PL011 中断收发升级；`qemu-virt` 链接与初始化至 `nShell`；新增主机单测与 QEMU 冒烟脚本 | `cmake --preset qemu-virt-arm64 && cmake --build --preset qemu-virt-arm64` 零错误零新增警告；`readelf` 验证 Higher-Half VMA + 严格 W^X + 2KB 向量对齐；新增 `run_arm64_mmu_test.py` 与 `run_arm64_gic_timer_test.py` 全通过；`qemu_arm64_smoke.py` 可直接在 QEMU 验证至 `nos# ` 交互 |
| **Stage 2**（后续迭代） | PSCI 多核 SMP（4 核 `MPIDR` 上线 + Per-CPU 负载均衡）；EL0 用户态与 `SVC #0` 系统调用闭环；`musl` 用户态运行时；双层 CNode + CDT 级联撤销；第一版 `Agent Cell`（预算+审计）与 `VMO` 零拷贝内存对象 | 4 核 SMP 启动与跨核调度稳定；EL0 服务通过 `SVC` 与能力隔离正常运行；超预算/非法能力访问由 EL1 硬拦截并写入审计记录 |
| **Stage 3**（后续迭代） | `RingIPC` 无锁 SQ/CQ + Schema 白名单；`VMO_KVCACHE` OS 级页表分页 + `clone_kv_prefix` + 协同驱逐；`Agent Cell` 亚毫秒 Snapshot/Rollback/Fork；`sched_infer` EDF/CBS + 惰性 SVE/SME 上下文 + big.LITTLE EAS + Gang 调度 | 多 Agent 并发工具调用吞吐与尾延迟达标；Cell 回滚耗时 `< 200 µs`；相同 System Prompt 前缀 KV-Cache 物理内存零冗余增长 |
| **Stage 4**（后续迭代） | `SMMUv3` SVA/PASID 统一地址空间；`kernel/accel/` 与 QEMU `virtio-accel` + RK3588 (`soc/rk3588/` + `rknpu`) 适配；ARMv9 PAC/BTI/MTE 安全硬化与 EL2 CCA Realm 扩展 | 加速器按 VA 直读共享权重与 KV 块且具备 IOMMU 跨 Cell 隔离；QEMU 与实板（RK3588）同套混合内核架构无缝运行 |

---

## 5. Stage 0 ~ Stage 1 关键工程设计与验证方案

### 5.1 隔离式 Freestanding 内核 Libc（解决纯裸机 `aarch64-elf-gcc` 缺库问题）

- **问题根因**：Homebrew `aarch64-elf-gcc`（GCC 15.1）仅包含编译器自带 freestanding 头（`stddef.h/stdint.h/stdbool.h/stdarg.h/limits.h`），不包含 Newlib/musl；而内核、`nShell` 与 VFS 使用 `<string.h>/<stdio.h>/<stdlib.h>/<errno.h>/<ctype.h>`。
- **合规解法**：
  - 严禁在 `include/` 根目录放置标准库同名头（遵守 `AGENTS.md`）。
  - 新增 `cmake/libc_detect.cmake` 检测工具链是否包含 `<string.h>` 并设置 `NOS_HAS_TOOLCHAIN_LIBC`。
  - 对 `arm-none-eabi-gcc`（`NOS_HAS_TOOLCHAIN_LIBC=ON`）：保持现有路径、Newlib `picolibc_newlib.c` 与 `-lgcc -lc -lm` **100% 不变**。
  - 对 `aarch64-elf-gcc`（`NOS_HAS_TOOLCHAIN_LIBC=OFF`）：向 `nos_headers` 添加 `-isystem libs/libc/freestanding/include`，链接改用 `-nostdlib` + `-lgcc`，并编译 `libs/libc/freestanding/klibc_runtime.c`（复用 `libs/string.c` 与 `libs/vsprintf.c`，补充堆桥接 `malloc/calloc/realloc/free`、`strtol/strtoul/qsort/printf/puts/putchar/errno`）。
  - 同时在 [libs/vsprintf.c](file:///Volumes/nick-disk/projects/nos/libs/vsprintf.c) 中对浮点 `%f` 支持增加 `#if !(defined(__aarch64__) && !defined(__ARM_FP))` 守卫，配合内核 `-mgeneral-regs-only` 实现零 SIMD 寄存器污染。

### 5.2 64 位内存管理与符号表修复

- 将 [include/kernel/mm.h](file:///Volumes/nick-disk/projects/nos/include/kernel/mm.h) 中 `struct mm_node::start_pfn` 与 `struct page::pfn` 升级为 `addr_t`（避免 Higher-Half 物理直映基址 `0xFFFFFF80_40200000` 右移 12 位后被 `u32` 截断）；
- 将 [kernel/mm/mm_node.c](file:///Volumes/nick-disk/projects/nos/kernel/mm/mm_node.c) 中页池与页面结构体大小计算从 `u32` 升级为 `size_t`；
- 将 [scripts/mkkallsyms.py](file:///Volumes/nick-disk/projects/nos/scripts/mkkallsyms.py) 输出的符号地址格式字面量附带 `UL` 后缀，避免 64 位符号地址常量溢出截断；
- 在 [kernel/task/task_dump.c](file:///Volumes/nick-disk/projects/nos/kernel/task/task_dump.c) 补全 `__aarch64__` 下基于 AAPCS64 帧指针（`x29/x30`）的任务栈回溯。

### 5.3 PL011 中断收发双模式兼容

- 保留 [drivers/serial/uart_pl011.c](file:///Volumes/nick-disk/projects/nos/drivers/serial/uart_pl011.c) 现有轮询与 Panic 接口，确保主机测试 [tests/host/run_pl011_uart_test.py](file:///Volumes/nick-disk/projects/nos/tests/host/run_pl011_uart_test.py) 无需改动且 100% 通过。
- 在 `CONFIG_ARCH_ARM64 && !defined(__HOST_TEST__)` 下：基址经 `arm64_ioremap(0x09000000UL, 0x1000UL)` 映射至高半核 MMIO 窗口；初始化时挂载 `kfifo` RX 缓冲与 `nos_notification`，开启 PL011 RXIM/RTIM 中断并注册 GIC SPI 33，使 `nShell` 可响应按键输入并在空闲时安全 `wfi` 休眠。

### 5.4 验证矩阵（Stage 0~1）

1. **构建验证**：`cmake --preset qemu-virt-arm64 && cmake --build --preset qemu-virt-arm64`（零错误、零新增警告）；
2. **ELF 布局与 W^X 核验**：`aarch64-elf-readelf -h -l -S build/qemu-virt-arm64/nos.elf` 检查入口 `0x40200000`、Higher-Half `0xFFFFFFF0...`、严格 W^X 段权限、2KB 对齐 `.vectors` 与无空洞 `nos.bin`；
3. **新增 ARM64 主机单测**：`tests/host/run_arm64_mmu_test.py`（页表 39/48-bit 编码、2MB/64KB CONT、W^X 强拒、ASID 世代回绕）与 `tests/host/run_arm64_gic_timer_test.py`（128-bit 纳秒防溢出、GIC IRQ 表路由、AI UAPI `_Static_assert` 尺寸检查）；
4. **全量回归与层隔离**：`tests/run_host_regressions.py`、`unittest discover`、`test_arch_flags.cmake`、`test_soc_headers.cmake`、`check_layer_deps.py` 以及 4 个 Cortex-M 预设全通过；
5. **QEMU 冒烟脚本**：`tests/qemu_arm64_smoke.py`（支持 GICv3 与 GICv2 验证 `Welcome to NOS:` 与 `nos# ` 交互命令行；若本机未装 `qemu-system-aarch64` 则报告环境缺失并给出复现指令）。
