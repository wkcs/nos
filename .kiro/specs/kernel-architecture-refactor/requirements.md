# 需求文档 - NOS 内核架构重构

## 简介

NOS (Nick Operating System) 是一个嵌入式操作系统内核，当前需要进行全面的架构重构以支持多架构、改进构建系统、增强模块化设计，并完善核心子系统功能。本重构旨在使系统更加灵活、可维护，并支持现代化的开发工具链和配置方式。

## 术语表

- **NOS**: Nick Operating System，目标操作系统内核
- **Kconfig**: Linux 内核配置系统，用于管理编译选项
- **DTS**: Device Tree Source，设备树源文件，用于描述硬件配置
- **MMU**: Memory Management Unit，内存管理单元
- **VFS**: Virtual File System，虚拟文件系统
- **BSP**: Board Support Package，板级支持包
- **Clang**: LLVM 编译器前端
- **ARM64**: 64 位 ARM 架构 (AArch64)
- **Shell**: 命令行交互界面
- **Scheduler**: 任务调度器

## 需求

### 需求 1: 构建系统现代化

**用户故事:** 作为内核开发者，我希望使用类似 Linux 的 Kconfig 配置系统，以便灵活管理编译选项和特性开关。

#### 验收标准

1. WHEN 开发者运行配置命令 THEN THE Build_System SHALL 提供基于 Kconfig 的配置界面
2. WHEN 配置选项被修改 THEN THE Build_System SHALL 生成 .config 文件和 autoconf.h 头文件
3. WHEN 执行构建 THEN THE Build_System SHALL 根据 .config 文件中的配置选择性编译模块
4. WHEN 配置依赖关系存在 THEN THE Build_System SHALL 自动处理选项间的依赖和冲突
5. THE Build_System SHALL 支持 menuconfig、defconfig 和 savedefconfig 命令

### 需求 2: 多编译器支持

**用户故事:** 作为内核开发者，我希望能够使用 Clang 编译器构建内核，以便利用现代编译器的优化和诊断功能。

#### 验收标准

1. WHEN 开发者指定使用 Clang THEN THE Build_System SHALL 使用 Clang/LLVM 工具链进行编译
2. WHEN 使用 Clang 编译 THEN THE Build_System SHALL 正确处理 Clang 特定的编译选项
3. WHEN 使用 GCC 编译 THEN THE Build_System SHALL 正确处理 GCC 特定的编译选项
4. THE Build_System SHALL 支持通过环境变量或配置选项切换编译器
5. WHEN 编译器不兼容的代码存在 THEN THE Build_System SHALL 通过条件编译隔离特定编译器代码

### 需求 3: 架构代码隔离

**用户故事:** 作为内核架构师，我希望所有架构相关的代码和配置只存在于 arch 目录下，以便实现清晰的架构抽象和多架构支持。

#### 验收标准

1. THE Kernel SHALL 将所有 ARM 特定代码放置在 arch/arm 目录下
2. THE Kernel SHALL 将所有 ARM64 特定代码放置在 arch/arm64 目录下
3. WHEN 添加新架构支持 THEN THE Kernel SHALL 只需在 arch/<arch_name> 目录下添加代码
4. THE Kernel SHALL 通过统一的架构抽象接口与架构特定代码交互
5. WHEN 架构特定配置存在 THEN THE Configuration SHALL 位于 arch/<arch_name>/Kconfig 文件中
6. THE Kernel SHALL 确保 board 目录不包含架构特定的底层代码

### 需求 4: 板级支持包重构

**用户故事:** 作为系统架构师，我希望评估并优化 board 目录的结构，以便减少代码重复并提高可维护性。

#### 验收标准

1. THE Board_Directory SHALL 只包含板级特定的配置和初始化代码
2. THE Board_Directory SHALL 将通用驱动代码移至 drivers 目录
3. THE Board_Directory SHALL 将架构相关代码移至 arch 目录
4. WHEN 多个板使用相同外设 THEN THE System SHALL 通过设备树或配置文件描述差异
5. THE Board_Directory SHALL 为每个板提供设备树文件或配置文件
6. THE Board_Directory SHALL 保持最小化的板级初始化代码

### 需求 5: MMU 支持配置

**用户故事:** 作为内核开发者，我希望能够配置是否启用 MMU，以便在不同的硬件平台和应用场景下灵活部署。

#### 验收标准

1. THE Kernel SHALL 提供 CONFIG_MMU 配置选项
2. WHEN CONFIG_MMU 启用 THEN THE Kernel SHALL 初始化并使用 MMU 进行内存管理
3. WHEN CONFIG_MMU 禁用 THEN THE Kernel SHALL 使用物理地址直接访问内存
4. THE Memory_Manager SHALL 提供统一的内存管理接口，隐藏 MMU 启用/禁用的差异
5. WHEN MMU 启用 THEN THE Kernel SHALL 支持虚拟地址映射和内存保护
6. WHEN MMU 禁用 THEN THE Kernel SHALL 提供简化的内存分配机制

### 需求 6: 设备树支持

**用户故事:** 作为板级开发者，我希望使用设备树描述硬件配置，以便在不修改代码的情况下支持不同的硬件变体。

#### 验收标准

1. THE Kernel SHALL 支持解析标准 DTS (Device Tree Source) 文件
2. WHEN 系统启动 THEN THE Kernel SHALL 从设备树中读取硬件配置信息
3. THE Kernel SHALL 提供设备树 API 供驱动程序查询硬件信息
4. WHEN 设备树中定义设备节点 THEN THE Kernel SHALL 自动创建对应的设备实例
5. THE Build_System SHALL 将 DTS 文件编译为 DTB (Device Tree Blob) 格式
6. THE Kernel SHALL 支持在运行时访问设备树属性

### 需求 7: 独立 C 运行时

**用户故事:** 作为内核开发者，我希望内核不依赖任何 C 标准库头文件，以便实现完全独立的运行环境。

#### 验收标准

1. THE Kernel SHALL 不包含任何标准 C 库头文件 (如 stdio.h, stdlib.h, string.h)
2. THE Kernel SHALL 提供自定义的类型定义 (在 include/kernel/types.h 中)
3. THE Kernel SHALL 实现必要的字符串和内存操作函数
4. THE Kernel SHALL 实现自定义的格式化输出函数
5. WHEN 需要标准 C 功能 THEN THE Kernel SHALL 提供内核专用的等效实现
6. THE Build_System SHALL 使用 -nostdinc 和 -nostdlib 编译选项

### 需求 8: 虚拟文件系统完善

**用户故事:** 作为应用开发者，我希望使用完善的虚拟文件系统，以便通过统一的接口访问不同类型的文件系统。

#### 验收标准

1. THE VFS SHALL 提供标准的文件操作接口 (open, close, read, write, ioctl)
2. THE VFS SHALL 支持挂载多个文件系统到不同的挂载点
3. THE VFS SHALL 支持至少以下文件系统类型: ramfs, procfs, sysfs, fatfs
4. WHEN 文件系统被挂载 THEN THE VFS SHALL 维护挂载点树结构
5. THE VFS SHALL 提供路径解析功能，支持绝对路径和相对路径
6. THE VFS SHALL 支持文件描述符管理和进程级文件表
7. THE VFS SHALL 提供 inode 和 dentry 缓存机制

### 需求 9: Shell 系统完善

**用户故事:** 作为系统管理员，我希望使用功能完善的 Shell，以便进行系统管理和调试。

#### 验收标准

1. THE Shell SHALL 提供交互式命令行界面
2. THE Shell SHALL 支持内置命令 (cd, ls, cat, echo, ps, help 等)
3. THE Shell SHALL 支持命令历史记录和命令行编辑
4. THE Shell SHALL 支持 Tab 键自动补全
5. THE Shell SHALL 支持管道和重定向操作
6. THE Shell SHALL 提供命令注册机制，允许动态添加新命令
7. THE Shell SHALL 支持脚本执行功能

### 需求 10: 调度器和任务管理完善

**用户故事:** 作为内核开发者，我希望拥有完善的调度器和任务管理系统，以便支持多任务和多线程应用。

#### 验收标准

1. THE Scheduler SHALL 支持多种调度策略 (优先级调度、时间片轮转、实时调度)
2. THE Scheduler SHALL 支持可配置的优先级级别
3. THE Task_Manager SHALL 支持任务创建、销毁、挂起和恢复
4. THE Task_Manager SHALL 支持线程创建和管理
5. THE Scheduler SHALL 提供 CPU 亲和性设置 (为多核做准备)
6. THE Task_Manager SHALL 维护任务状态 (运行、就绪、阻塞、退出)
7. THE Scheduler SHALL 支持任务间同步原语 (互斥锁、信号量、消息队列)
8. THE Task_Manager SHALL 提供任务统计信息 (CPU 使用率、运行时间等)

### 需求 11: ARM64 架构支持

**用户故事:** 作为内核开发者，我希望添加 ARM64 架构支持，以便在 64 位 ARM 平台上运行内核。

#### 验收标准

1. THE Kernel SHALL 在 arch/arm64 目录下实现 ARM64 架构支持
2. THE Kernel SHALL 实现 ARM64 的异常处理和中断处理
3. THE Kernel SHALL 实现 ARM64 的上下文切换机制
4. THE Kernel SHALL 实现 ARM64 的 MMU 初始化和页表管理
5. THE Kernel SHALL 支持 ARM64 的启动流程 (从 bootloader 到内核)
6. THE Build_System SHALL 支持为 ARM64 目标编译内核
7. THE Kernel SHALL 实现 ARM64 特定的系统调用接口
8. THE Kernel SHALL 支持 GIC-v3 (Generic Interrupt Controller version 3) 中断控制器
9. WHEN 使用 GIC-v3 THEN THE Kernel SHALL 正确初始化 Distributor、Redistributor 和 CPU Interface
10. THE Kernel SHALL 支持 GIC-v3 的中断路由和亲和性配置
11. THE Kernel SHALL 支持 GIC-v3 的 SGI (软件生成中断) 用于核间通信

### 需求 12: 中断管理系统完善

**用户故事:** 作为驱动开发者，我希望使用完善的中断管理系统，以便高效处理硬件中断。

#### 验收标准

1. THE IRQ_Manager SHALL 提供中断注册和注销接口
2. THE IRQ_Manager SHALL 支持中断优先级配置
3. THE IRQ_Manager SHALL 支持中断嵌套控制
4. THE IRQ_Manager SHALL 提供中断使能和禁用接口
5. THE IRQ_Manager SHALL 支持共享中断处理
6. THE IRQ_Manager SHALL 维护中断统计信息 (中断次数、处理时间等)
7. THE IRQ_Manager SHALL 支持软中断和 tasklet 机制
8. THE IRQ_Manager SHALL 提供架构无关的中断抽象层

### 需求 13: 设备驱动模型完善

**用户故事:** 作为驱动开发者，我希望使用完善的设备驱动模型，以便快速开发和集成新的设备驱动。

#### 验收标准

1. THE Driver_Model SHALL 提供统一的设备注册和注销接口
2. THE Driver_Model SHALL 支持设备和驱动的自动匹配机制
3. THE Driver_Model SHALL 提供设备类型分类 (字符设备、块设备、网络设备等)
4. THE Driver_Model SHALL 支持设备树驱动的设备探测
5. THE Driver_Model SHALL 提供设备电源管理接口
6. THE Driver_Model SHALL 支持热插拔设备
7. THE Driver_Model SHALL 提供总线抽象层 (platform bus, I2C bus, SPI bus 等)
8. THE Driver_Model SHALL 在 sysfs 中导出设备信息


### 需求 14: 内存管理系统完善

**用户故事:** 作为内核开发者，我希望拥有完善的内存管理系统，以便高效管理物理和虚拟内存。

#### 验收标准

1. THE Memory_Manager SHALL 实现 Buddy 系统进行页级内存分配
2. THE Memory_Manager SHALL 实现 Slab 分配器进行小对象缓存
3. THE Memory_Manager SHALL 提供 kmalloc/kfree 接口进行通用内存分配
4. WHEN CONFIG_MMU 启用 THEN THE Memory_Manager SHALL 提供 vmalloc/vfree 接口进行虚拟内存分配
5. THE Memory_Manager SHALL 支持不同的内存分配标志 (GFP_KERNEL, GFP_ATOMIC, GFP_DMA)
6. THE Memory_Manager SHALL 维护页帧描述符和引用计数
7. THE Memory_Manager SHALL 支持内存页的合并和分裂 (Buddy 算法)
8. THE Memory_Manager SHALL 提供内存使用统计信息

### 需求 15: 同步原语完善

**用户故事:** 作为内核开发者，我希望拥有完善的同步原语，以便正确处理并发和竞态条件。

#### 验收标准

1. THE Kernel SHALL 提供自旋锁 (spinlock) 用于短时间临界区保护
2. THE Kernel SHALL 提供读写自旋锁 (rwlock) 支持多读者单写者模式
3. THE Kernel SHALL 提供互斥锁 (mutex) 用于可睡眠的临界区保护
4. THE Kernel SHALL 提供信号量 (semaphore) 用于资源计数和同步
5. THE Kernel SHALL 提供完成量 (completion) 用于事件通知
6. THE Kernel SHALL 提供读写信号量 (rw_semaphore) 支持可睡眠的多读者单写者模式
7. THE Kernel SHALL 提供等待队列 (wait_queue) 用于任务等待和唤醒
8. THE Kernel SHALL 支持自旋锁与中断禁用的组合 (spin_lock_irqsave)
9. WHEN 使用同步原语 THEN THE Kernel SHALL 正确处理死锁检测 (在 DEBUG 模式下)
10. THE Kernel SHALL 提供内存屏障指令确保内存访问顺序

### 需求 16: 原子操作支持

**用户故事:** 作为内核开发者，我希望使用原子操作，以便实现无锁数据结构和算法。

#### 验收标准

1. THE Kernel SHALL 提供原子读写操作 (atomic_read, atomic_set)
2. THE Kernel SHALL 提供原子算术操作 (atomic_add, atomic_sub, atomic_inc, atomic_dec)
3. THE Kernel SHALL 提供原子比较交换操作 (atomic_cmpxchg)
4. THE Kernel SHALL 提供原子测试并设置操作 (atomic_test_and_set)
5. THE Kernel SHALL 提供原子位操作 (set_bit, clear_bit, test_bit)
6. THE Kernel SHALL 确保原子操作在多核环境下的正确性
7. THE Kernel SHALL 提供内存屏障 (mb, rmb, wmb, smp_mb)
8. THE Kernel SHALL 提供编译器屏障防止指令重排
