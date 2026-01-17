# 实施计划: NOS 内核架构重构

## 概述

本实施计划将 NOS 内核架构重构分解为可执行的编码任务。每个任务都引用具体的需求，并包含清晰的实施目标。任务按照依赖关系组织，确保增量式开发和持续验证。

## 任务

- [x] 1. 建立 Kconfig 配置系统基础
  - 创建顶层 Kconfig 文件
  - 创建 arch/Kconfig 用于架构选择
  - 实现配置解析脚本生成 .config 和 autoconf.h
  - 添加 menuconfig、defconfig、savedefconfig 支持
  - _需求: 1.1, 1.2, 1.3, 1.4, 1.5_

- [x] 2. 重构构建系统支持多编译器
  - [x] 2.1 修改顶层 Makefile 支持编译器选择
    - 添加 CONFIG_CC_IS_CLANG 配置选项
    - 实现编译器检测和工具链设置
    - _需求: 2.1, 2.4_
  
  - [x] 2.2 添加 Clang 特定编译选项
    - 配置 Clang 编译标志
    - 处理 Clang 与 GCC 的差异
    - _需求: 2.2_
  
  - [x] 2.3 添加 GCC 特定编译选项
    - 配置 GCC 编译标志
    - 确保 GCC 兼容性
    - _需求: 2.3_
  
  - [x] 2.4 编写属性测试验证编译器切换
    - **属性 6: 编译器切换机制**
    - **验证需求: 2.4**

- [ ] 3. 检查点 - 确保构建系统正常工作
  - 确保所有测试通过，询问用户是否有问题


- [x] 4. 重组目录结构
  - [x] 4.1 创建新的 arch 目录结构
    - 为 ARM 和 ARM64 创建标准目录布局
    - 创建 arch/arm/Kconfig 和 arch/arm64/Kconfig
    - _需求: 3.1, 3.2, 3.5_
  
  - [x] 4.2 移动架构特定代码到 arch 目录
    - 识别并移动 ARM 特定代码到 arch/arm
    - 识别并移动 ARM64 特定代码到 arch/arm64
    - _需求: 3.1, 3.2_
  
  - [x] 4.3 精简 board 目录
    - 将通用驱动代码移至 drivers 目录
    - 将架构相关代码移至 arch 目录
    - 只保留板级配置和初始化代码
    - _需求: 4.1, 4.2, 4.3_
  
  - [x] 4.4 为每个板创建设备树文件
    - 创建 board/*/board.dts 文件
    - 添加 DTB 编译支持到 Makefile
    - _需求: 4.5, 6.5_
  
  - [x] 4.5 编写单元测试验证目录结构
    - 测试架构代码隔离
    - 测试 board 目录内容限制
    - _需求: 3.1, 3.2, 3.6, 4.1_

- [-] 5. 实现架构抽象层接口
  - [ ] 5.1 定义中断管理抽象接口
    - 创建 include/asm-generic/irq.h
    - 定义 arch_irq_init, arch_irq_enable, arch_irq_disable
    - _需求: 3.4, 12.8_
  
  - [ ] 5.2 定义 MMU 管理抽象接口
    - 创建 include/asm-generic/mmu.h
    - 定义 arch_mmu_init, arch_map_page, arch_unmap_page
    - 支持 CONFIG_MMU 条件编译
    - _需求: 3.4, 5.1, 5.4_
  
  - [ ] 5.3 定义上下文切换抽象接口
    - 创建 include/asm-generic/switch.h
    - 定义 arch_switch_to, arch_task_init
    - _需求: 3.4_
  
  - [ ] 5.4 定义原子操作抽象接口
    - 创建 include/asm-generic/atomic.h
    - 定义原子读写、算术、比较交换操作
    - _需求: 3.4, 16.1, 16.2, 16.3_
  
  - [ ] 5.5 编写属性测试验证架构抽象
    - **属性 10: 架构抽象接口使用**
    - **验证需求: 3.4**

- [ ] 6. 检查点 - 确保架构抽象层定义完整
  - 确保所有测试通过，询问用户是否有问题

- [ ] 7. 实现独立 C 运行时
  - [ ] 7.1 创建内核类型定义
    - 创建 include/kernel/types.h
    - 定义 u8, u16, u32, u64, size_t 等类型
    - _需求: 7.2_
  
  - [ ] 7.2 实现字符串操作函数
    - 实现 strlen, strcpy, strcmp, strcat 等
    - 创建 lib/string.c
    - _需求: 7.3_
  
  - [ ] 7.3 实现内存操作函数
    - 实现 memset, memcpy, memmove, memcmp
    - 添加到 lib/string.c
    - _需求: 7.3_
  
  - [ ] 7.4 实现格式化输出函数
    - 实现 printk, snprintf, vsnprintf
    - 创建 kernel/printk.c
    - _需求: 7.4_
  
  - [ ] 7.5 编写属性测试验证字符串函数等效性
    - **属性 27: 字符串函数功能等效性**
    - **验证需求: 7.3, 7.5**
  
  - [ ] 7.6 编写属性测试验证格式化输出
    - **属性 28: 格式化输出功能**
    - **验证需求: 7.4, 7.5**
  
  - [ ] 7.7 配置构建系统使用 -nostdinc -nostdlib
    - 修改 Makefile 添加编译选项
    - 验证不包含标准库头文件
    - _需求: 7.1, 7.6_

- [ ] 8. 实现内存管理子系统
  - [ ] 8.1 实现 Buddy 系统
    - 创建 kernel/mm/buddy.c
    - 实现 alloc_pages, free_pages
    - 实现页帧合并和分裂算法
    - _需求: 14.1, 14.6, 14.7_
  
  - [ ] 8.2 实现 Slab 分配器
    - 创建 kernel/mm/slab.c
    - 实现 kmem_cache_create, kmem_cache_alloc, kmem_cache_free
    - _需求: 14.2_
  
  - [ ] 8.3 实现通用内存分配接口
    - 实现 kmalloc, kfree, kzalloc
    - 支持 GFP_KERNEL, GFP_ATOMIC, GFP_DMA 标志
    - _需求: 14.3, 14.5_
  
  - [ ] 8.4 实现虚拟内存分配 (CONFIG_MMU=y)
    - 创建 kernel/mm/vmalloc.c
    - 实现 vmalloc, vfree
    - _需求: 14.4_
  
  - [ ] 8.5 编写属性测试验证 Buddy 系统
    - **属性 67: Buddy 分配和释放**
    - **验证需求: 14.1, 14.7**
  
  - [ ] 8.6 编写属性测试验证 kmalloc/kfree
    - **属性 69: kmalloc/kfree 配对**
    - **验证需求: 14.3**

- [ ] 9. 检查点 - 确保内存管理系统正常工作
  - 确保所有测试通过，询问用户是否有问题


- [ ] 10. 实现同步原语
  - [ ] 10.1 实现自旋锁
    - 创建 kernel/sync/spinlock.c
    - 实现 spin_lock, spin_unlock, spin_trylock
    - 实现 spin_lock_irqsave, spin_unlock_irqrestore
    - _需求: 15.1, 15.8_
  
  - [ ] 10.2 实现读写自旋锁
    - 创建 kernel/sync/rwlock.c
    - 实现 read_lock, read_unlock, write_lock, write_unlock
    - _需求: 15.2_
  
  - [ ] 10.3 实现互斥锁
    - 创建 kernel/sync/mutex.c
    - 实现 mutex_lock, mutex_unlock, mutex_trylock
    - _需求: 15.3_
  
  - [ ] 10.4 实现信号量
    - 创建 kernel/sync/semaphore.c
    - 实现 sem_wait, sem_post, sem_trywait
    - _需求: 15.4_
  
  - [ ] 10.5 实现完成量
    - 创建 kernel/sync/completion.c
    - 实现 wait_for_completion, complete, complete_all
    - _需求: 15.5_
  
  - [ ] 10.6 实现等待队列
    - 创建 kernel/sync/wait.c
    - 实现 wait_event, wake_up, wake_up_all
    - _需求: 15.7_
  
  - [ ] 10.7 编写属性测试验证自旋锁互斥性
    - **属性 73: 自旋锁互斥性**
    - **验证需求: 15.1**
  
  - [ ] 10.8 编写属性测试验证互斥锁睡眠
    - **属性 75: 互斥锁睡眠**
    - **验证需求: 15.3**
  
  - [ ] 10.9 编写属性测试验证信号量计数
    - **属性 76: 信号量计数**
    - **验证需求: 15.4**

- [ ] 11. 实现原子操作和内存屏障
  - [ ] 11.1 实现 ARM 原子操作
    - 创建 arch/arm/include/asm/atomic.h
    - 实现 atomic_read, atomic_set, atomic_add, atomic_cmpxchg
    - _需求: 16.1, 16.2, 16.3_
  
  - [ ] 11.2 实现 ARM64 原子操作
    - 创建 arch/arm64/include/asm/atomic.h
    - 使用 LDXR/STXR 指令实现原子操作
    - _需求: 16.1, 16.2, 16.3_
  
  - [ ] 11.3 实现内存屏障
    - 创建 include/asm-generic/barrier.h
    - 定义 mb, rmb, wmb, smp_mb
    - _需求: 16.7_
  
  - [ ] 11.4 编写属性测试验证原子比较交换
    - **属性 82: 原子比较交换**
    - **验证需求: 16.3**

- [ ] 12. 实现 ARM 架构支持
  - [ ] 12.1 实现 ARM 启动代码
    - 创建 arch/arm/boot/boot.S
    - 实现从 bootloader 到内核的跳转
    - _需求: 3.1_
  
  - [ ] 12.2 实现 ARM 异常处理
    - 创建 arch/arm/kernel/entry.S
    - 实现异常向量表和处理程序
    - _需求: 3.1_
  
  - [ ] 12.3 实现 ARM 中断处理
    - 创建 arch/arm/kernel/irq.c
    - 实现 arch_irq_init, arch_irq_enable, arch_irq_disable
    - _需求: 3.1_
  
  - [ ] 12.4 实现 ARM MMU 管理
    - 创建 arch/arm/mm/mmu.c
    - 实现 arch_mmu_init, arch_map_page
    - _需求: 3.1, 5.2_
  
  - [ ] 12.5 实现 ARM 上下文切换
    - 创建 arch/arm/kernel/process.c
    - 实现 arch_switch_to
    - _需求: 3.1_

- [ ] 13. 检查点 - 确保 ARM 架构支持完整
  - 确保所有测试通过，询问用户是否有问题

- [ ] 14. 实现 ARM64 架构支持
  - [ ] 14.1 实现 ARM64 启动代码
    - 创建 arch/arm64/boot/boot.S
    - 配置 EL1 并跳转到内核
    - _需求: 11.1, 11.5_
  
  - [ ] 14.2 实现 ARM64 异常处理
    - 创建 arch/arm64/kernel/entry.S
    - 实现异常向量表（4 个异常级别）
    - _需求: 11.2_
  
  - [ ] 14.3 实现 ARM64 GIC-v3 驱动
    - 创建 arch/arm64/kernel/gic-v3.c
    - 实现 Distributor、Redistributor、CPU Interface 初始化
    - 实现中断使能、禁用、优先级配置
    - 实现 SGI 发送功能
    - _需求: 11.8, 11.9, 11.10, 11.11_
  
  - [ ] 14.4 实现 ARM64 MMU 管理
    - 创建 arch/arm64/mm/mmu.c
    - 实现 3 级页表管理
    - 配置 MAIR_EL1, TCR_EL1, TTBR0_EL1, TTBR1_EL1
    - _需求: 11.4_
  
  - [ ] 14.5 实现 ARM64 上下文切换
    - 创建 arch/arm64/kernel/process.c
    - 保存/恢复 x19-x28, fp, lr, sp
    - _需求: 11.3_
  
  - [ ] 14.6 实现 ARM64 系统调用接口
    - 创建 arch/arm64/kernel/syscall.c
    - 实现 SVC 处理和参数传递
    - _需求: 11.7_
  
  - [ ] 14.7 编写属性测试验证 GIC-v3 初始化
    - **属性 51: GIC-v3 初始化**
    - **验证需求: 11.8, 11.9**
  
  - [ ] 14.8 编写属性测试验证 ARM64 上下文切换
    - **属性 48: ARM64 上下文切换**
    - **验证需求: 11.3**

- [ ] 15. 实现设备树支持
  - [ ] 15.1 实现设备树解析器
    - 创建 kernel/of/fdt.c
    - 实现 DTB 格式解析
    - 构建设备树节点树
    - _需求: 6.1, 6.2_
  
  - [ ] 15.2 实现设备树 API
    - 创建 kernel/of/base.c
    - 实现 of_find_node_by_path, of_find_compatible_node
    - 实现 of_property_read_u32, of_property_read_string
    - _需求: 6.3, 6.6_
  
  - [ ] 15.3 集成设备树到设备模型
    - 实现设备树驱动的设备探测
    - 自动创建 platform_device
    - _需求: 6.4_
  
  - [ ] 15.4 编写属性测试验证设备树解析
    - **属性 22: 设备树解析正确性**
    - **验证需求: 6.1**
  
  - [ ] 15.5 编写属性测试验证设备自动创建
    - **属性 24: 设备自动创建**
    - **验证需求: 6.4**

- [ ] 16. 检查点 - 确保设备树支持正常工作
  - 确保所有测试通过，询问用户是否有问题


- [ ] 17. 实现中断管理系统
  - [ ] 17.1 实现中断描述符管理
    - 创建 kernel/irq/irqdesc.c
    - 实现中断描述符数组和分配
    - _需求: 12.1_
  
  - [ ] 17.2 实现中断注册和注销
    - 创建 kernel/irq/manage.c
    - 实现 request_irq, free_irq
    - 支持共享中断
    - _需求: 12.1, 12.5_
  
  - [ ] 17.3 实现中断优先级和嵌套
    - 实现中断优先级配置
    - 实现中断嵌套控制
    - _需求: 12.2, 12.3_
  
  - [ ] 17.4 实现软中断和 Tasklet
    - 创建 kernel/irq/softirq.c
    - 实现 raise_softirq, tasklet_schedule
    - _需求: 12.7_
  
  - [ ] 17.5 实现中断统计
    - 维护中断次数和处理时间
    - 提供查询接口
    - _需求: 12.6_
  
  - [ ] 17.6 编写属性测试验证中断注册
    - **属性 51: 中断注册和注销**（注意：这里的属性 51 在更新后变成了 GIC-v3，需要调整编号）
    - **验证需求: 12.1**
  
  - [ ] 17.7 编写属性测试验证共享中断
    - **属性 55: 共享中断处理**
    - **验证需求: 12.5**

- [ ] 18. 实现设备驱动模型
  - [ ] 18.1 实现设备和驱动核心
    - 创建 drivers/base/core.c
    - 实现 device_register, driver_register
    - 实现设备和驱动匹配机制
    - _需求: 13.1, 13.2_
  
  - [ ] 18.2 实现总线抽象
    - 创建 drivers/base/bus.c
    - 实现 platform_bus, i2c_bus, spi_bus
    - _需求: 13.7_
  
  - [ ] 18.3 实现设备类型分类
    - 实现字符设备、块设备、网络设备分类
    - _需求: 13.3_
  
  - [ ] 18.4 实现设备电源管理
    - 创建 drivers/base/power.c
    - 实现设备挂起和恢复接口
    - _需求: 13.5_
  
  - [ ] 18.5 实现热插拔支持
    - 实现设备插入和拔出事件处理
    - _需求: 13.6_
  
  - [ ] 18.6 编写属性测试验证设备驱动匹配
    - **属性 60: 设备驱动自动匹配**
    - **验证需求: 13.2**

- [ ] 19. 实现虚拟文件系统 (VFS)
  - [ ] 19.1 实现 VFS 核心数据结构
    - 创建 fs/vfs/inode.c
    - 实现 inode, dentry, file 结构
    - _需求: 8.1_
  
  - [ ] 19.2 实现文件操作接口
    - 创建 fs/vfs/file.c
    - 实现 sys_open, sys_close, sys_read, sys_write
    - _需求: 8.1_
  
  - [ ] 19.3 实现路径解析
    - 创建 fs/vfs/namei.c
    - 实现绝对路径和相对路径解析
    - _需求: 8.5_
  
  - [ ] 19.4 实现文件系统挂载
    - 创建 fs/vfs/mount.c
    - 实现 sys_mount, sys_umount
    - 维护挂载点树
    - _需求: 8.2, 8.4_
  
  - [ ] 19.5 实现文件描述符管理
    - 创建 fs/vfs/fd.c
    - 实现进程级文件描述符表
    - _需求: 8.6_
  
  - [ ] 19.6 实现 inode 和 dentry 缓存
    - 创建 fs/vfs/dcache.c
    - 实现 LRU 缓存机制
    - _需求: 8.7_
  
  - [ ] 19.7 实现 ramfs 文件系统
    - 创建 fs/ramfs/inode.c
    - 实现内存文件系统
    - _需求: 8.3_
  
  - [ ] 19.8 实现 procfs 文件系统
    - 创建 fs/procfs/inode.c
    - 导出进程和系统信息
    - _需求: 8.3_
  
  - [ ] 19.9 实现 sysfs 文件系统
    - 创建 fs/sysfs/inode.c
    - 导出设备和驱动信息
    - _需求: 8.3, 13.8_
  
  - [ ] 19.10 编写属性测试验证路径解析
    - **属性 31: 路径解析正确性**
    - **验证需求: 8.5**
  
  - [ ] 19.11 编写属性测试验证文件描述符管理
    - **属性 32: 文件描述符管理**
    - **验证需求: 8.6**

- [ ] 20. 检查点 - 确保 VFS 正常工作
  - 确保所有测试通过，询问用户是否有问题

- [ ] 21. 实现调度器和任务管理
  - [ ] 21.1 实现任务控制块
    - 创建 kernel/sched/task.c
    - 定义 task_struct 结构
    - _需求: 10.3_
  
  - [ ] 21.2 实现调度器核心
    - 创建 kernel/sched/core.c
    - 实现 schedule() 函数
    - 实现优先级调度算法
    - _需求: 10.1, 10.2_
  
  - [ ] 21.3 实现任务创建和销毁
    - 实现 task_create, task_destroy
    - 分配任务栈和初始化上下文
    - _需求: 10.3_
  
  - [ ] 21.4 实现任务状态管理
    - 实现任务挂起、恢复、阻塞
    - 维护任务状态转换
    - _需求: 10.3, 10.6_
  
  - [ ] 21.5 实现线程支持
    - 实现线程创建和管理
    - 共享地址空间
    - _需求: 10.4_
  
  - [ ] 21.6 实现 CPU 亲和性
    - 实现 set_task_cpu, get_task_cpu
    - _需求: 10.5_
  
  - [ ] 21.7 实现任务统计
    - 维护 CPU 使用率和运行时间
    - _需求: 10.8_
  
  - [ ] 21.8 编写属性测试验证优先级调度
    - **属性 40: 优先级调度顺序**
    - **验证需求: 10.2**
  
  - [ ] 21.9 编写属性测试验证任务状态一致性
    - **属性 44: 任务状态一致性**
    - **验证需求: 10.6**

- [ ] 22. 实现 Shell 系统
  - [ ] 22.1 实现 Shell 核心
    - 创建 kernel/shell/shell.c
    - 实现命令行读取和解析
    - _需求: 9.1_
  
  - [ ] 22.2 实现内置命令
    - 实现 cd, ls, cat, echo, ps, help 等命令
    - _需求: 9.2_
  
  - [ ] 22.3 实现命令历史
    - 实现历史记录存储和导航
    - _需求: 9.3_
  
  - [ ] 22.4 实现 Tab 自动补全
    - 实现命令和文件名补全
    - _需求: 9.4_
  
  - [ ] 22.5 实现管道和重定向
    - 实现管道符 | 和重定向 >, <
    - _需求: 9.5_
  
  - [ ] 22.6 实现命令注册机制
    - 允许动态添加新命令
    - _需求: 9.6_
  
  - [ ] 22.7 编写属性测试验证管道和重定向
    - **属性 36: 管道和重定向**
    - **验证需求: 9.5**

- [ ] 23. 检查点 - 确保所有核心子系统正常工作
  - 确保所有测试通过，询问用户是否有问题

- [ ] 24. 创建 QEMU 测试环境
  - [ ] 24.1 创建 QEMU ARM 配置
    - 创建 board/qemu-virt/qemu-virt-arm.dts
    - 配置 UART、Timer、中断控制器
    - _需求: 所有需求_
  
  - [ ] 24.2 创建 QEMU ARM64 配置
    - 创建 board/qemu-virt/qemu-virt-arm64.dts
    - 配置 GIC-v3、UART、Timer
    - _需求: 11.8, 11.9_
  
  - [ ] 24.3 实现 QEMU 测试套件
    - 创建 kernel/test/qemu_test.c
    - 实现自动化测试入口
    - _需求: 所有需求_
  
  - [ ] 24.4 创建 QEMU 测试脚本
    - 创建 scripts/qemu_test.sh
    - 支持 ARM 和 ARM64 测试
    - _需求: 所有需求_
  
  - [ ] 24.5 添加 QEMU 测试到 Makefile
    - 添加 qemu-test, qemu-run, qemu-debug 目标
    - _需求: 所有需求_

- [ ] 25. 集成测试和验证
  - [ ] 25.1 在 QEMU ARM 上运行测试
    - 验证所有功能在 ARM 平台正常工作
    - _需求: 所有需求_
  
  - [ ] 25.2 在 QEMU ARM64 上运行测试
    - 验证所有功能在 ARM64 平台正常工作
    - 特别验证 GIC-v3 功能
    - _需求: 11.8, 11.9, 11.10, 11.11_
  
  - [ ] 25.3 运行所有属性测试
    - 确保所有属性测试通过
    - _需求: 所有需求_
  
  - [ ] 25.4 运行所有单元测试
    - 确保所有单元测试通过
    - _需求: 所有需求_

- [ ] 26. 最终检查点 - 完成重构
  - 确保所有测试通过
  - 验证在 QEMU 上成功运行
  - 询问用户是否有问题或需要调整

## 注意事项

- 每个任务都引用了具体的需求以确保可追溯性
- 检查点任务确保增量验证
- 属性测试验证通用正确性属性
- 单元测试验证具体示例和边界情况
- QEMU 是主要的测试和验证平台
- 所有测试任务都是必需的，确保从一开始就全面测试
