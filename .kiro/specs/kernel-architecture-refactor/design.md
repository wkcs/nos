# 设计文档 - NOS 内核架构重构

## 概述

本设计文档描述了 NOS (Nick Operating System) 内核的全面架构重构方案。重构的核心目标是建立一个模块化、可扩展、支持多架构的现代化嵌入式操作系统内核。

### 设计目标

1. **架构独立性**: 通过清晰的抽象层实现架构无关的内核核心
2. **可配置性**: 使用 Kconfig 系统实现灵活的特性配置
3. **可维护性**: 减少代码重复，提高代码组织结构
4. **可扩展性**: 便于添加新架构、新驱动和新文件系统
5. **标准兼容**: 遵循 POSIX 和 Linux 内核的设计模式

### 重构范围

- 构建系统 (Kconfig + Makefile)
- 目录结构重组
- 架构抽象层
- 设备驱动模型
- 虚拟文件系统
- 任务调度和管理
- 中断管理系统
- ARM64 架构支持

## 架构

### 整体架构

```
┌─────────────────────────────────────────────────────────┐
│                    用户空间应用                          │
└─────────────────────────────────────────────────────────┘
                          ↕
┌─────────────────────────────────────────────────────────┐
│                    系统调用接口                          │
└─────────────────────────────────────────────────────────┘
                          ↕
┌─────────────────────────────────────────────────────────┐
│  内核核心层 (架构无关)                                   │
│  ┌──────────┬──────────┬──────────┬──────────┐         │
│  │ 调度器   │ 内存管理 │   VFS    │  设备模型│         │
│  └──────────┴──────────┴──────────┴──────────┘         │
└─────────────────────────────────────────────────────────┘
                          ↕
┌─────────────────────────────────────────────────────────┐
│  架构抽象层 (HAL)                                        │
│  ┌──────────┬──────────┬──────────┬──────────┐         │
│  │ 中断管理 │ MMU 管理 │ 上下文   │ 定时器   │         │
│  └──────────┴──────────┴──────────┴──────────┘         │
└─────────────────────────────────────────────────────────┘
                          ↕
┌─────────────────────────────────────────────────────────┐
│  架构特定层 (arch/arm, arch/arm64)                       │
└─────────────────────────────────────────────────────────┘
                          ↕
┌─────────────────────────────────────────────────────────┐
│  板级支持层 (board/*)                                    │
└─────────────────────────────────────────────────────────┘
```

### 分层设计原则

1. **内核核心层**: 完全架构无关，只依赖架构抽象层接口
2. **架构抽象层**: 定义统一接口，隐藏架构差异
3. **架构特定层**: 实现具体架构的底层功能
4. **板级支持层**: 只包含板级配置和初始化


## 组件和接口

### 1. 构建系统 (Kconfig + Makefile)

#### 1.1 Kconfig 配置系统

**目录结构:**
```
Kconfig                    # 顶层配置文件
arch/Kconfig              # 架构选择
arch/arm/Kconfig          # ARM 特定配置
arch/arm64/Kconfig        # ARM64 特定配置
kernel/Kconfig            # 内核核心配置
drivers/Kconfig           # 驱动配置
fs/Kconfig                # 文件系统配置
```

**核心配置选项:**
- `CONFIG_ARCH`: 架构选择 (ARM, ARM64)
- `CONFIG_MMU`: MMU 支持开关
- `CONFIG_DEVICE_TREE`: 设备树支持
- `CONFIG_CC_IS_CLANG`: 编译器选择
- `CONFIG_SMP`: 多核支持 (预留)

**接口:**
```c
// 生成的配置头文件: include/generated/autoconf.h
#define CONFIG_MMU 1
#define CONFIG_ARCH_ARM 1
#define CONFIG_MAX_PRIORITY 32
```

#### 1.2 Makefile 系统

**顶层 Makefile 结构:**
```makefile
# 编译器选择
ifeq ($(CONFIG_CC_IS_CLANG),y)
    CC = clang
    LD = ld.lld
else
    CC = $(CROSS_COMPILE)gcc
    LD = $(CROSS_COMPILE)ld
endif

# 通用编译选项
CFLAGS = -nostdinc -nostdlib -ffreestanding
CFLAGS += -I$(srctree)/include
CFLAGS += -I$(srctree)/arch/$(ARCH)/include
CFLAGS += -include include/generated/autoconf.h

# 架构特定选项
include arch/$(ARCH)/Makefile
```

**构建目标:**
- `make menuconfig`: 图形化配置界面
- `make defconfig`: 使用默认配置
- `make savedefconfig`: 保存最小配置
- `make`: 构建内核
- `make dtbs`: 编译设备树

### 2. 目录结构重组

**新的目录结构:**
```
nos/
├── arch/                      # 架构特定代码
│   ├── arm/
│   │   ├── Kconfig           # ARM 配置
│   │   ├── Makefile          # ARM 构建规则
│   │   ├── include/          # ARM 头文件
│   │   │   └── asm/          # 汇编接口
│   │   ├── kernel/           # ARM 内核代码
│   │   │   ├── entry.S       # 异常入口
│   │   │   ├── irq.c         # 中断处理
│   │   │   └── mmu.c         # MMU 管理
│   │   ├── mm/               # ARM 内存管理
│   │   └── boot/             # ARM 启动代码
│   └── arm64/
│       ├── Kconfig
│       ├── Makefile
│       ├── include/
│       ├── kernel/
│       ├── mm/
│       └── boot/
├── board/                     # 板级支持 (精简)
│   ├── nk60/
│   │   ├── board.c           # 板级初始化
│   │   ├── board.dts         # 设备树
│   │   └── defconfig         # 默认配置
│   └── qemu-virt/
├── drivers/                   # 设备驱动
│   ├── base/                 # 驱动核心
│   ├── char/                 # 字符设备
│   ├── block/                # 块设备
│   ├── i2c/
│   ├── spi/
│   └── usb/
├── fs/                        # 文件系统
│   ├── vfs/                  # VFS 核心
│   ├── ramfs/
│   ├── procfs/
│   ├── sysfs/
│   └── fatfs/
├── include/                   # 公共头文件
│   ├── kernel/               # 内核 API
│   ├── asm-generic/          # 通用汇编接口
│   └── generated/            # 生成的头文件
├── init/                      # 初始化代码
├── kernel/                    # 内核核心
│   ├── sched/                # 调度器
│   ├── irq/                  # 中断管理
│   ├── mm/                   # 内存管理
│   └── time/                 # 时间管理
├── lib/                       # 内核库函数
├── scripts/                   # 构建脚本
└── tools/                     # 开发工具
```

**关键变化:**
1. `arch/` 目录包含所有架构特定代码
2. `board/` 目录大幅精简，只保留板级配置
3. 驱动代码从 `board/` 移至 `drivers/`
4. 新增 `include/asm-generic/` 用于通用接口定义


### 7. Shell 系统

#### 7.1 Shell 核心结构

**头文件: include/kernel/shell.h**
```c
/* 命令处理函数类型 */
typedef int (*shell_cmd_handler_t)(int argc, char **argv);

/* 命令结构 */
struct shell_command {
    const char *name;
    const char *help;
    shell_cmd_handler_t handler;
    struct list_head list;
};

/* Shell 上下文 */
struct shell_context {
    char *input_buffer;
    size_t buffer_size;
    size_t cursor_pos;
    char **history;
    int history_size;
    int history_index;
    char *cwd;  /* 当前工作目录 */
};

/* Shell 接口 */
void shell_init(void);
int shell_register_command(struct shell_command *cmd);
void shell_unregister_command(const char *name);
void shell_run(void);
```

#### 7.2 内置命令

**实现文件: kernel/shell/builtins.c**
```c
/* 内置命令列表 */
- cd: 切换目录
- ls: 列出文件
- cat: 显示文件内容
- echo: 输出文本
- ps: 显示进程列表
- kill: 终止进程
- mount: 挂载文件系统
- umount: 卸载文件系统
- help: 显示帮助信息
- clear: 清屏
- pwd: 显示当前目录
```

#### 7.3 命令解析器

**实现逻辑:**
```c
/* 命令行解析流程 */
1. 读取输入行
2. 处理转义字符和引号
3. 分割命令和参数
4. 处理管道符 (|)
5. 处理重定向 (>, <, >>)
6. 查找并执行命令
7. 返回执行结果
```

### 8. 中断管理系统

#### 8.1 中断描述符

**头文件: include/kernel/irq.h**
```c
/* 中断标志 */
#define IRQF_SHARED     0x00000001  /* 共享中断 */
#define IRQF_TRIGGER_RISING  0x00000002
#define IRQF_TRIGGER_FALLING 0x00000004

/* 中断统计信息 */
struct irq_stat {
    unsigned long count;
    unsigned long last_time;
    unsigned long total_time;
};

/* 中断描述符 */
struct irq_desc {
    unsigned int irq;
    const char *name;
    irq_handler_t handler;
    void *dev_id;
    unsigned long flags;
    int priority;
    struct irq_stat stat;
    struct list_head list;
};

/* 软中断 */
struct softirq_action {
    void (*action)(void *);
    void *data;
};

/* Tasklet */
struct tasklet_struct {
    void (*func)(unsigned long);
    unsigned long data;
    atomic_t count;
    struct list_head list;
};
```

#### 8.2 中断管理接口

**实现文件: kernel/irq/manage.c**
```c
/* 中断管理 */
int request_irq(unsigned int irq, irq_handler_t handler,
                unsigned long flags, const char *name, void *dev_id);
void free_irq(unsigned int irq, void *dev_id);
void enable_irq(unsigned int irq);
void disable_irq(unsigned int irq);
void disable_irq_nosync(unsigned int irq);

/* 软中断 */
void open_softirq(int nr, void (*action)(void *));
void raise_softirq(int nr);

/* Tasklet */
void tasklet_init(struct tasklet_struct *t, void (*func)(unsigned long),
                  unsigned long data);
void tasklet_schedule(struct tasklet_struct *t);
void tasklet_kill(struct tasklet_struct *t);
```

### 9. ARM64 架构支持

#### 9.1 ARM64 目录结构

```
arch/arm64/
├── Kconfig                    # ARM64 配置
├── Makefile                   # ARM64 构建规则
├── include/
│   └── asm/
│       ├── processor.h        # 处理器定义
│       ├── ptrace.h          # 寄存器定义
│       ├── page.h            # 页定义
│       ├── mmu.h             # MMU 定义
│       ├── irq.h             # 中断定义
│       └── gic.h             # GIC 定义
├── kernel/
│   ├── head.S                # 启动代码
│   ├── entry.S               # 异常入口
│   ├── irq.c                 # 中断处理
│   ├── gic-v3.c              # GIC-v3 驱动
│   ├── process.c             # 进程管理
│   └── syscall.c             # 系统调用
├── mm/
│   ├── init.c                # 内存初始化
│   ├── mmu.c                 # MMU 管理
│   └── fault.c               # 页错误处理
└── boot/
    └── boot.S                # 引导代码
```

#### 9.2 ARM64 关键实现

**异常向量表 (arch/arm64/kernel/entry.S):**
```asm
.align 11
.global vectors
vectors:
    /* Current EL with SP0 */
    .align 7
    b sync_exception_sp0
    .align 7
    b irq_exception_sp0
    .align 7
    b fiq_exception_sp0
    .align 7
    b serror_exception_sp0
    
    /* Current EL with SPx */
    .align 7
    b sync_exception_spx
    .align 7
    b irq_exception_spx
    .align 7
    b fiq_exception_spx
    .align 7
    b serror_exception_spx
```

**MMU 初始化 (arch/arm64/mm/mmu.c):**
```c
/* ARM64 页表级别 */
#define ARM64_HW_PGTABLE_LEVELS 3

/* 页表项标志 */
#define PTE_VALID       (1 << 0)
#define PTE_TYPE_PAGE   (3 << 0)
#define PTE_AF          (1 << 10)  /* Access Flag */
#define PTE_SHARED      (3 << 8)
#define PTE_WRITE       (0 << 7)   /* AP[2:1] = 00 */
#define PTE_USER        (1 << 6)   /* AP[1] */

void arm64_mmu_init(void) {
    /* 1. 创建页表 */
    /* 2. 映射内核空间 */
    /* 3. 配置 MAIR_EL1 (内存属性) */
    /* 4. 配置 TCR_EL1 (转换控制) */
    /* 5. 设置 TTBR0_EL1 和 TTBR1_EL1 */
    /* 6. 启用 MMU (SCTLR_EL1.M = 1) */
}
```

**上下文切换 (arch/arm64/kernel/process.c):**
```c
void arch_switch_to(struct task_struct *prev, struct task_struct *next) {
    /* 保存 prev 的上下文 */
    /* x19-x28, fp, lr, sp */
    
    /* 切换页表 (如果需要) */
    if (prev->mm != next->mm) {
        /* 切换 TTBR0_EL1 */
    }
    
    /* 恢复 next 的上下文 */
}
```

#### 9.3 GIC-v3 中断控制器支持

**GIC-v3 架构概述:**

GIC-v3 (Generic Interrupt Controller version 3) 是 ARM64 平台的标准中断控制器，支持：
- 最多 1020 个共享外设中断 (SPI)
- 每个 CPU 核心 16 个私有外设中断 (PPI)
- 每个 CPU 核心 16 个软件生成中断 (SGI)
- 中断路由和亲和性
- 中断优先级和抢占
- 消息信号中断 (MSI)

**GIC-v3 寄存器接口:**

```c
/* arch/arm64/include/asm/gic.h */

/* GIC Distributor (GICD) 寄存器 */
#define GICD_CTLR           0x0000  /* 控制寄存器 */
#define GICD_TYPER          0x0004  /* 类型寄存器 */
#define GICD_IIDR           0x0008  /* 实现标识寄存器 */
#define GICD_IGROUPR(n)     (0x0080 + (n) * 4)  /* 中断组寄存器 */
#define GICD_ISENABLER(n)   (0x0100 + (n) * 4)  /* 中断使能设置寄存器 */
#define GICD_ICENABLER(n)   (0x0180 + (n) * 4)  /* 中断使能清除寄存器 */
#define GICD_ISPENDR(n)     (0x0200 + (n) * 4)  /* 中断挂起设置寄存器 */
#define GICD_ICPENDR(n)     (0x0280 + (n) * 4)  /* 中断挂起清除寄存器 */
#define GICD_ISACTIVER(n)   (0x0300 + (n) * 4)  /* 中断活动设置寄存器 */
#define GICD_ICACTIVER(n)   (0x0380 + (n) * 4)  /* 中断活动清除寄存器 */
#define GICD_IPRIORITYR(n)  (0x0400 + (n) * 4)  /* 中断优先级寄存器 */
#define GICD_ITARGETSR(n)   (0x0800 + (n) * 4)  /* 中断目标寄存器 */
#define GICD_ICFGR(n)       (0x0C00 + (n) * 4)  /* 中断配置寄存器 */
#define GICD_IROUTER(n)     (0x6000 + (n) * 8)  /* 中断路由寄存器 (GICv3) */

/* GIC CPU Interface (GICC) 系统寄存器 (GICv3) */
#define ICC_CTLR_EL1        S3_0_C12_C12_4  /* 控制寄存器 */
#define ICC_PMR_EL1         S3_0_C4_C6_0    /* 优先级掩码寄存器 */
#define ICC_IAR1_EL1        S3_0_C12_C12_0  /* 中断确认寄存器 */
#define ICC_EOIR1_EL1       S3_0_C12_C12_1  /* 中断结束寄存器 */
#define ICC_IGRPEN1_EL1     S3_0_C12_C12_7  /* 中断组 1 使能寄存器 */
#define ICC_SRE_EL1         S3_0_C12_C12_5  /* 系统寄存器使能 */

/* GIC Redistributor (GICR) 寄存器 */
#define GICR_CTLR           0x0000  /* 控制寄存器 */
#define GICR_IIDR           0x0004  /* 实现标识寄存器 */
#define GICR_TYPER          0x0008  /* 类型寄存器 */
#define GICR_WAKER          0x0014  /* 唤醒寄存器 */
#define GICR_IGROUPR0       0x0080  /* SGI/PPI 中断组寄存器 */
#define GICR_ISENABLER0     0x0100  /* SGI/PPI 中断使能设置寄存器 */
#define GICR_ICENABLER0     0x0180  /* SGI/PPI 中断使能清除寄存器 */
#define GICR_ISPENDR0       0x0200  /* SGI/PPI 中断挂起设置寄存器 */
#define GICR_ICPENDR0       0x0280  /* SGI/PPI 中断挂起清除寄存器 */
#define GICR_IPRIORITYR(n)  (0x0400 + (n) * 4)  /* SGI/PPI 优先级寄存器 */
#define GICR_ICFGR0         0x0C00  /* SGI 配置寄存器 */
#define GICR_ICFGR1         0x0C04  /* PPI 配置寄存器 */

/* 中断类型 */
#define GIC_SGI_BASE        0       /* 软件生成中断 (0-15) */
#define GIC_PPI_BASE        16      /* 私有外设中断 (16-31) */
#define GIC_SPI_BASE        32      /* 共享外设中断 (32-1019) */

/* 中断优先级 */
#define GIC_PRIO_HIGHEST    0x00
#define GIC_PRIO_HIGH       0x40
#define GIC_PRIO_NORMAL     0x80
#define GIC_PRIO_LOW        0xC0
#define GIC_PRIO_LOWEST     0xFF
```

**GIC-v3 初始化 (arch/arm64/kernel/gic-v3.c):**

```c
/* GIC-v3 配置结构 */
struct gic_chip_data {
    void __iomem *dist_base;    /* Distributor 基地址 */
    void __iomem *redist_base;  /* Redistributor 基地址 */
    u32 nr_irqs;                /* 中断数量 */
};

static struct gic_chip_data gic_data;

/* 初始化 GIC Distributor */
static void gic_dist_init(void)
{
    u32 typer, nr_irqs;
    
    /* 禁用 Distributor */
    writel(0, gic_data.dist_base + GICD_CTLR);
    
    /* 读取支持的中断数量 */
    typer = readl(gic_data.dist_base + GICD_TYPER);
    nr_irqs = ((typer & 0x1F) + 1) * 32;
    gic_data.nr_irqs = nr_irqs;
    
    pr_info("GICv3: %d interrupts supported\n", nr_irqs);
    
    /* 配置所有 SPI 为 Group 1 */
    for (u32 i = 32; i < nr_irqs; i += 32) {
        writel(0xFFFFFFFF, gic_data.dist_base + GICD_IGROUPR(i / 32));
    }
    
    /* 禁用所有 SPI */
    for (u32 i = 32; i < nr_irqs; i += 32) {
        writel(0xFFFFFFFF, gic_data.dist_base + GICD_ICENABLER(i / 32));
    }
    
    /* 清除所有 SPI 挂起状态 */
    for (u32 i = 32; i < nr_irqs; i += 32) {
        writel(0xFFFFFFFF, gic_data.dist_base + GICD_ICPENDR(i / 32));
    }
    
    /* 设置所有 SPI 优先级为默认值 */
    for (u32 i = 32; i < nr_irqs; i += 4) {
        writel(0xA0A0A0A0, gic_data.dist_base + GICD_IPRIORITYR(i / 4));
    }
    
    /* 配置所有 SPI 为边沿触发 */
    for (u32 i = 32; i < nr_irqs; i += 16) {
        writel(0xAAAAAAAA, gic_data.dist_base + GICD_ICFGR(i / 16));
    }
    
    /* 启用 Distributor (支持 Group 1) */
    writel(GICD_CTLR_ENABLE_G1 | GICD_CTLR_ARE_NS, 
           gic_data.dist_base + GICD_CTLR);
}

/* 初始化 GIC Redistributor (每个 CPU 核心) */
static void gic_redist_init(void)
{
    void __iomem *redist_base = gic_data.redist_base;
    
    /* 唤醒 Redistributor */
    u32 waker = readl(redist_base + GICR_WAKER);
    waker &= ~GICR_WAKER_ProcessorSleep;
    writel(waker, redist_base + GICR_WAKER);
    
    /* 等待唤醒完成 */
    while (readl(redist_base + GICR_WAKER) & GICR_WAKER_ChildrenAsleep)
        cpu_relax();
    
    /* 配置 SGI/PPI 为 Group 1 */
    writel(0xFFFFFFFF, redist_base + GICR_IGROUPR0);
    
    /* 禁用所有 SGI/PPI */
    writel(0xFFFFFFFF, redist_base + GICR_ICENABLER0);
    
    /* 清除所有 SGI/PPI 挂起状态 */
    writel(0xFFFFFFFF, redist_base + GICR_ICPENDR0);
    
    /* 设置 SGI/PPI 优先级 */
    for (u32 i = 0; i < 32; i += 4) {
        writel(0xA0A0A0A0, redist_base + GICR_IPRIORITYR(i / 4));
    }
    
    /* 配置 PPI 为边沿触发 */
    writel(0xAAAAAAAA, redist_base + GICR_ICFGR1);
}

/* 初始化 GIC CPU Interface */
static void gic_cpu_init(void)
{
    u64 val;
    
    /* 启用系统寄存器访问 */
    val = read_sysreg(ICC_SRE_EL1);
    val |= ICC_SRE_EL1_SRE;
    write_sysreg(val, ICC_SRE_EL1);
    isb();
    
    /* 设置优先级掩码 (允许所有优先级) */
    write_sysreg(0xFF, ICC_PMR_EL1);
    
    /* 启用 Group 1 中断 */
    write_sysreg(1, ICC_IGRPEN1_EL1);
}

/* GIC-v3 主初始化函数 */
void gic_init(void)
{
    /* 从设备树获取 GIC 基地址 */
    struct device_node *node = of_find_compatible_node("arm,gic-v3");
    if (node == NULL) {
        pr_err("GICv3: device tree node not found\n");
        return;
    }
    
    /* 获取 Distributor 基地址 */
    u64 dist_base;
    of_property_read_u64(node, "reg", &dist_base);
    gic_data.dist_base = ioremap(dist_base, 0x10000);
    
    /* 获取 Redistributor 基地址 */
    u64 redist_base;
    of_property_read_u64_index(node, "reg", 1, &redist_base);
    gic_data.redist_base = ioremap(redist_base, 0x20000);
    
    /* 初始化 GIC */
    gic_dist_init();
    gic_redist_init();
    gic_cpu_init();
    
    pr_info("GICv3: initialized\n");
}

/* 使能中断 */
void gic_enable_irq(unsigned int irq)
{
    if (irq < 32) {
        /* SGI/PPI - 使用 Redistributor */
        writel(1 << (irq % 32), gic_data.redist_base + GICR_ISENABLER0);
    } else {
        /* SPI - 使用 Distributor */
        writel(1 << (irq % 32), 
               gic_data.dist_base + GICD_ISENABLER(irq / 32));
    }
}

/* 禁用中断 */
void gic_disable_irq(unsigned int irq)
{
    if (irq < 32) {
        /* SGI/PPI - 使用 Redistributor */
        writel(1 << (irq % 32), gic_data.redist_base + GICR_ICENABLER0);
    } else {
        /* SPI - 使用 Distributor */
        writel(1 << (irq % 32), 
               gic_data.dist_base + GICD_ICENABLER(irq / 32));
    }
}

/* 设置中断优先级 */
void gic_set_priority(unsigned int irq, u8 priority)
{
    void __iomem *base;
    u32 offset;
    
    if (irq < 32) {
        /* SGI/PPI - 使用 Redistributor */
        base = gic_data.redist_base;
        offset = GICR_IPRIORITYR(irq / 4);
    } else {
        /* SPI - 使用 Distributor */
        base = gic_data.dist_base;
        offset = GICD_IPRIORITYR(irq / 4);
    }
    
    u32 shift = (irq % 4) * 8;
    u32 val = readl(base + offset);
    val &= ~(0xFF << shift);
    val |= (priority << shift);
    writel(val, base + offset);
}

/* 设置中断路由 (SPI only) */
void gic_set_affinity(unsigned int irq, u64 affinity)
{
    if (irq < 32)
        return;  /* SGI/PPI 不支持路由 */
    
    writeq(affinity, gic_data.dist_base + GICD_IROUTER(irq));
}

/* 中断处理入口 */
void gic_handle_irq(void)
{
    u32 irqstat;
    
    /* 读取中断确认寄存器 */
    irqstat = read_sysreg(ICC_IAR1_EL1);
    u32 irqnr = irqstat & 0x3FF;
    
    if (irqnr >= 1020) {
        /* 伪中断 */
        return;
    }
    
    /* 调用中断处理函数 */
    handle_domain_irq(irqnr);
    
    /* 写入中断结束寄存器 */
    write_sysreg(irqstat, ICC_EOIR1_EL1);
}

/* 发送 SGI (核间中断) */
void gic_send_sgi(unsigned int sgi, u64 target_list)
{
    u64 val;
    
    val = (target_list << 16) | sgi;
    write_sysreg(val, ICC_SGI1R_EL1);
    isb();
}
```

**设备树配置示例:**

```dts
/* board/qemu-virt/qemu-virt-arm64.dts */
/ {
    compatible = "linux,dummy-virt";
    #address-cells = <2>;
    #size-cells = <2>;
    
    interrupt-parent = <&gic>;
    
    gic: interrupt-controller@8000000 {
        compatible = "arm,gic-v3";
        #interrupt-cells = <3>;
        interrupt-controller;
        reg = <0x0 0x08000000 0 0x10000>,  /* GICD */
              <0x0 0x080A0000 0 0xF60000>; /* GICR */
    };
    
    timer {
        compatible = "arm,armv8-timer";
        interrupts = <1 13 0xf08>,  /* Secure Physical Timer */
                     <1 14 0xf08>,  /* Non-secure Physical Timer */
                     <1 11 0xf08>,  /* Virtual Timer */
                     <1 10 0xf08>;  /* Hypervisor Timer */
    };
    
    uart0: serial@9000000 {
        compatible = "arm,pl011";
        reg = <0x0 0x09000000 0x0 0x1000>;
        interrupts = <0 1 4>;  /* SPI 1, IRQ */
    };
};
```

### 10. 独立 C 运行时

#### 10.1 类型定义

**头文件: include/kernel/types.h**
```c
/* 基本类型 */
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned int       u32;
typedef unsigned long long u64;

typedef signed char        s8;
typedef signed short       s16;
typedef signed int         s32;
typedef signed long long   s64;

/* 标准类型 */
typedef u32 size_t;
typedef s32 ssize_t;
typedef s32 off_t;
typedef s32 pid_t;
typedef u32 mode_t;
typedef u32 uid_t;
typedef u32 gid_t;

/* 布尔类型 */
typedef int bool;
#define true  1
#define false 0

/* NULL 定义 */
#define NULL ((void *)0)
```

#### 10.2 字符串和内存函数

**头文件: include/kernel/string.h**
```c
/* 字符串操作 */
size_t strlen(const char *s);
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t n);
int strcmp(const char *s1, const char *s2);
int strncmp(const char *s1, const char *s2, size_t n);
char *strcat(char *dest, const char *src);
char *strchr(const char *s, int c);
char *strrchr(const char *s, int c);

/* 内存操作 */
void *memset(void *s, int c, size_t n);
void *memcpy(void *dest, const void *src, size_t n);
void *memmove(void *dest, const void *src, size_t n);
int memcmp(const void *s1, const void *s2, size_t n);
```

#### 10.3 格式化输出

**头文件: include/kernel/printk.h**
```c
/* 内核日志级别 */
#define KERN_EMERG   "<0>"  /* 系统不可用 */
#define KERN_ALERT   "<1>"  /* 必须立即采取行动 */
#define KERN_CRIT    "<2>"  /* 严重情况 */
#define KERN_ERR     "<3>"  /* 错误情况 */
#define KERN_WARNING "<4>"  /* 警告情况 */
#define KERN_NOTICE  "<5>"  /* 正常但重要的情况 */
#define KERN_INFO    "<6>"  /* 信息性消息 */
#define KERN_DEBUG   "<7>"  /* 调试级别消息 */

/* 格式化输出 */
int printk(const char *fmt, ...);
int snprintf(char *buf, size_t size, const char *fmt, ...);
int vsnprintf(char *buf, size_t size, const char *fmt, va_list args);

/* 便捷宏 */
#define pr_emerg(fmt, ...)   printk(KERN_EMERG fmt, ##__VA_ARGS__)
#define pr_alert(fmt, ...)   printk(KERN_ALERT fmt, ##__VA_ARGS__)
#define pr_crit(fmt, ...)    printk(KERN_CRIT fmt, ##__VA_ARGS__)
#define pr_err(fmt, ...)     printk(KERN_ERR fmt, ##__VA_ARGS__)
#define pr_warning(fmt, ...) printk(KERN_WARNING fmt, ##__VA_ARGS__)
#define pr_notice(fmt, ...)  printk(KERN_NOTICE fmt, ##__VA_ARGS__)
#define pr_info(fmt, ...)    printk(KERN_INFO fmt, ##__VA_ARGS__)
#define pr_debug(fmt, ...)   printk(KERN_DEBUG fmt, ##__VA_ARGS__)
```


## 数据模型

### 1. 内存布局

#### 1.1 虚拟地址空间布局 (ARM64, CONFIG_MMU=y)

```
0xFFFF_FFFF_FFFF_FFFF  ┌─────────────────────┐
                       │   内核空间          │
                       │   (TTBR1_EL1)       │
0xFFFF_0000_0000_0000  ├─────────────────────┤
                       │   空洞              │
0x0000_FFFF_FFFF_FFFF  ├─────────────────────┤
                       │   用户空间          │
                       │   (TTBR0_EL1)       │
0x0000_0000_0000_0000  └─────────────────────┘

内核空间详细布局:
0xFFFF_FFFF_FFFF_FFFF  ┌─────────────────────┐
                       │   固定映射区        │
0xFFFF_FFFF_F000_0000  ├─────────────────────┤
                       │   vmalloc 区        │
0xFFFF_FF80_0000_0000  ├─────────────────────┤
                       │   内核模块区        │
0xFFFF_FF00_0000_0000  ├─────────────────────┤
                       │   内核直接映射区    │
0xFFFF_8000_0000_0000  └─────────────────────┘
```

#### 1.2 物理内存布局

```
物理地址              用途
0x0000_0000          ┌─────────────────────┐
                     │   保留区            │
0x0000_1000          ├─────────────────────┤
                     │   内核代码段        │
                     │   (.text)           │
                     ├─────────────────────┤
                     │   内核只读数据      │
                     │   (.rodata)         │
                     ├─────────────────────┤
                     │   内核数据段        │
                     │   (.data)           │
                     ├─────────────────────┤
                     │   内核 BSS 段       │
                     │   (.bss)            │
_end                 ├─────────────────────┤
                     │   动态内存池        │
                     │   (buddy + slab)    │
                     ├─────────────────────┤
                     │   设备树 (DTB)      │
                     ├─────────────────────┤
                     │   保留内存          │
                     │   (DMA, 帧缓冲等)   │
RAM_END              └─────────────────────┘
```

### 2. 进程/任务模型

#### 2.1 任务状态转换图

```
                    创建
                     ↓
    ┌──────────→ [就绪] ←──────────┐
    │               ↓               │
    │            调度选中            │
    │               ↓               │
    │           [运行中]            │
    │               ↓               │
    │         时间片用完/抢占        │
    │               ↓               │
    └───────────────┴───────────────┘
                    ↓
              等待事件/资源
                    ↓
                [阻塞]
                    ↓
              事件发生/资源可用
                    ↓
                [就绪]
                    
                [运行中]
                    ↓
                  退出
                    ↓
                [僵尸]
                    ↓
                父进程回收
                    ↓
                  销毁
```

#### 2.2 调度队列结构

```
优先级队列 (多级反馈队列):

优先级 0 (最高)  [task1] → [task2] → NULL
优先级 1         [task3] → NULL
优先级 2         [task4] → [task5] → [task6] → NULL
...
优先级 31 (最低) [taskN] → NULL

实时队列 (FIFO/RR):
FIFO 队列        [rt_task1] → [rt_task2] → NULL
RR 队列          [rr_task1] → [rr_task2] → NULL
```

### 3. 文件系统模型

#### 3.1 VFS 层次结构

```
                    [super_block]
                          ↓
                    [dentry root]
                          ↓
        ┌─────────────────┼─────────────────┐
        ↓                 ↓                 ↓
   [dentry /dev]    [dentry /proc]    [dentry /mnt]
        ↓                 ↓                 ↓
    [inode]           [inode]           [inode]
        ↓                 ↓                 ↓
   [ramfs ops]       [procfs ops]      [fatfs ops]
```

#### 3.2 文件描述符表

```
进程 A:
fd_table[0] → stdin  → [file] → [dentry] → [inode]
fd_table[1] → stdout → [file] → [dentry] → [inode]
fd_table[2] → stderr → [file] → [dentry] → [inode]
fd_table[3] → file1  → [file] → [dentry] → [inode]
...

进程 B:
fd_table[0] → stdin  → [file] → [dentry] → [inode]
fd_table[1] → stdout → [file] → [dentry] → [inode]
fd_table[2] → stderr → [file] → [dentry] → [inode]
fd_table[3] → file2  → [file] → [dentry] → [inode]
...
```

### 4. 设备模型

#### 4.1 设备树结构

```
设备树示例 (board.dts):

/ {
    compatible = "vendor,board-name";
    #address-cells = <1>;
    #size-cells = <1>;
    
    cpus {
        #address-cells = <1>;
        #size-cells = <0>;
        
        cpu@0 {
            compatible = "arm,cortex-a53";
            device_type = "cpu";
            reg = <0>;
        };
    };
    
    memory@40000000 {
        device_type = "memory";
        reg = <0x40000000 0x20000000>;  /* 512MB */
    };
    
    soc {
        #address-cells = <1>;
        #size-cells = <1>;
        compatible = "simple-bus";
        ranges;
        
        uart0: serial@40011000 {
            compatible = "vendor,uart";
            reg = <0x40011000 0x400>;
            interrupts = <37>;
            clock-frequency = <48000000>;
        };
        
        i2c0: i2c@40005400 {
            compatible = "vendor,i2c";
            reg = <0x40005400 0x400>;
            interrupts = <31>;
            #address-cells = <1>;
            #size-cells = <0>;
            
            eeprom@50 {
                compatible = "atmel,24c256";
                reg = <0x50>;
            };
        };
    };
};
```

#### 4.2 设备驱动绑定流程

```
1. 系统启动
   ↓
2. 解析设备树
   ↓
3. 创建 device_node 树
   ↓
4. 遍历设备节点
   ↓
5. 对每个节点:
   - 创建 platform_device
   - 设置 device->of_node
   - 调用 device_register()
   ↓
6. 设备注册触发匹配:
   - 遍历已注册的驱动
   - 比较 compatible 字符串
   - 匹配成功则调用 driver->probe()
   ↓
7. probe() 函数:
   - 从设备树读取配置
   - 初始化硬件
   - 注册字符设备/块设备
   - 创建 sysfs 节点
```


## 正确性属性

*属性是一个特征或行为，应该在系统的所有有效执行中保持为真——本质上是关于系统应该做什么的形式化陈述。属性作为人类可读规范和机器可验证正确性保证之间的桥梁。*

### 构建系统属性

**属性 1: 配置修改生成输出文件**
*对于任意*配置修改操作，系统应该生成 .config 文件和 autoconf.h 头文件
**验证需求: 1.2**

**属性 2: 构建结果与配置一致**
*对于任意*有效的 .config 配置文件，构建输出应该只包含配置中启用的模块
**验证需求: 1.3**

**属性 3: 依赖关系自动解析**
*对于任意*具有依赖关系的配置选项集合，Kconfig 系统应该自动启用所有必需的依赖项或禁用冲突的选项
**验证需求: 1.4**

**属性 4: Clang 编译选项正确性**
*对于任意*使用 Clang 编译的源文件，生成的编译命令应该包含 Clang 特定的选项并排除 GCC 专用选项
**验证需求: 2.2**

**属性 5: GCC 编译选项正确性**
*对于任意*使用 GCC 编译的源文件，生成的编译命令应该包含 GCC 特定的选项并排除 Clang 专用选项
**验证需求: 2.3**

**属性 6: 编译器切换机制**
*对于任意*编译器选择（通过环境变量或配置），构建系统应该使用对应的工具链
**验证需求: 2.4**

**属性 7: 条件编译隔离**
*对于任意*编译器特定的代码块，预处理后的代码应该只包含当前编译器支持的代码
**验证需求: 2.5**

### 架构抽象属性

**属性 8: ARM 代码隔离**
*对于任意*ARM 特定的代码文件，其路径应该以 arch/arm/ 开头
**验证需求: 3.1**

**属性 9: ARM64 代码隔离**
*对于任意*ARM64 特定的代码文件，其路径应该以 arch/arm64/ 开头
**验证需求: 3.2**

**属性 10: 架构抽象接口使用**
*对于任意*内核核心层的代码文件，其中的架构相关调用应该只使用架构抽象接口
**验证需求: 3.4**

**属性 11: 架构配置位置**
*对于任意*架构特定的 Kconfig 选项，其定义应该位于 arch/<arch_name>/Kconfig 文件中
**验证需求: 3.5**

**属性 12: Board 目录代码限制**
*对于任意*board 目录下的代码文件，不应包含架构特定的底层操作（如 MMU 操作、中断控制器操作）
**验证需求: 3.6**

### 板级支持属性

**属性 13: Board 目录内容限制**
*对于任意*board 目录下的代码，应该只包含板级配置、初始化和设备树文件
**验证需求: 4.1**

**属性 14: 通用驱动位置**
*对于任意*通用设备驱动代码，其路径应该以 drivers/ 开头而不是 board/
**验证需求: 4.2**

**属性 15: 架构代码位置**
*对于任意*架构相关的代码，其路径应该以 arch/ 开头而不是 board/
**验证需求: 4.3**

**属性 16: 板级设备树存在性**
*对于任意*board 目录下的板级配置，应该存在对应的 .dts 文件或配置文件
**验证需求: 4.5**

### MMU 支持属性

**属性 17: MMU 启用时的初始化**
*对于任意*启用 CONFIG_MMU 的配置，内核启动后 MMU 应该处于启用状态
**验证需求: 5.2**

**属性 18: MMU 禁用时的地址访问**
*对于任意*禁用 CONFIG_MMU 的配置，虚拟地址到物理地址的转换应该是恒等映射
**验证需求: 5.3**

**属性 19: 内存管理接口一致性**
*对于任意*内存分配请求，无论 CONFIG_MMU 启用或禁用，内存管理 API 的行为应该保持一致
**验证需求: 5.4**

**属性 20: MMU 虚拟地址映射**
*对于任意*启用 MMU 的配置，创建的虚拟地址映射应该正确反映指定的访问权限
**验证需求: 5.5**

**属性 21: MMU 禁用时的内存分配**
*对于任意*禁用 MMU 的配置，内存分配应该返回物理地址
**验证需求: 5.6**

### 设备树属性

**属性 22: 设备树解析正确性**
*对于任意*符合标准格式的 DTS 文件，解析后的设备树结构应该正确反映 DTS 中定义的层次关系和属性
**验证需求: 6.1**

**属性 23: 设备树 API 功能性**
*对于任意*设备树节点和属性，通过 API 查询应该返回与设备树定义一致的值
**验证需求: 6.3**

**属性 24: 设备自动创建**
*对于任意*设备树中定义的设备节点，系统启动后应该存在对应的设备实例
**验证需求: 6.4**

**属性 25: 设备树运行时访问**
*对于任意*设备树属性，在运行时通过 API 访问应该返回正确的值
**验证需求: 6.6**

### 独立 C 运行时属性

**属性 26: 标准库头文件排除**
*对于任意*内核源文件，不应包含标准 C 库头文件（stdio.h, stdlib.h, string.h 等）
**验证需求: 7.1**

**属性 27: 字符串函数功能等效性**
*对于任意*字符串操作，内核实现的函数应该与标准 C 库函数行为一致
**验证需求: 7.3, 7.5**

**属性 28: 格式化输出功能**
*对于任意*格式化字符串和参数，printk/snprintf 应该生成与标准 printf/snprintf 相同的输出
**验证需求: 7.4, 7.5**

### 虚拟文件系统属性

**属性 29: 多文件系统挂载**
*对于任意*多个文件系统挂载操作，每个文件系统应该正确挂载到指定的挂载点
**验证需求: 8.2**

**属性 30: 挂载点树维护**
*对于任意*文件系统挂载操作，VFS 应该维护正确的挂载点树结构
**验证需求: 8.4**

**属性 31: 路径解析正确性**
*对于任意*有效的文件路径（绝对或相对），VFS 应该正确解析到对应的 inode
**验证需求: 8.5**

**属性 32: 文件描述符管理**
*对于任意*进程的文件操作，文件描述符应该正确映射到对应的文件对象
**验证需求: 8.6**

**属性 33: 缓存一致性**
*对于任意*文件访问操作，inode 和 dentry 缓存应该与实际文件系统状态保持一致
**验证需求: 8.7**

### Shell 系统属性

**属性 34: 命令历史功能**
*对于任意*已执行的命令序列，使用上下箭头键应该能够正确导航历史记录
**验证需求: 9.3**

**属性 35: Tab 自动补全**
*对于任意*部分命令或文件名，按 Tab 键应该提供正确的补全建议
**验证需求: 9.4**

**属性 36: 管道和重定向**
*对于任意*包含管道或重定向的命令，Shell 应该正确连接输入输出流
**验证需求: 9.5**

**属性 37: 命令注册机制**
*对于任意*新注册的命令，应该能够通过命令名正确调用
**验证需求: 9.6**

**属性 38: 脚本执行**
*对于任意*有效的 Shell 脚本，执行结果应该与逐行执行命令的结果一致
**验证需求: 9.7**

### 调度器和任务管理属性

**属性 39: 调度策略行为**
*对于任意*调度策略（NORMAL, FIFO, RR），任务调度应该遵循该策略的规则
**验证需求: 10.1**

**属性 40: 优先级调度顺序**
*对于任意*就绪任务集合，调度器应该选择优先级最高的任务运行
**验证需求: 10.2**

**属性 41: 任务生命周期管理**
*对于任意*任务，创建、挂起、恢复、销毁操作应该正确改变任务状态
**验证需求: 10.3**

**属性 42: 线程管理**
*对于任意*线程创建操作，新线程应该共享父任务的地址空间但拥有独立的栈
**验证需求: 10.4**

**属性 43: CPU 亲和性**
*对于任意*设置了 CPU 亲和性的任务，应该只在指定的 CPU 上运行
**验证需求: 10.5**

**属性 44: 任务状态一致性**
*对于任意*任务，其状态应该与实际运行状态一致（运行、就绪、阻塞、退出）
**验证需求: 10.6**

**属性 45: 同步原语正确性**
*对于任意*使用互斥锁、信号量或消息队列的任务，同步行为应该正确防止竞态条件
**验证需求: 10.7**

**属性 46: 任务统计准确性**
*对于任意*任务，统计信息（CPU 使用率、运行时间）应该准确反映实际执行情况
**验证需求: 10.8**

### ARM64 架构属性

**属性 47: ARM64 异常处理**
*对于任意*ARM64 异常（同步异常、IRQ、FIQ），异常处理程序应该正确保存和恢复上下文
**验证需求: 11.2**

**属性 48: ARM64 上下文切换**
*对于任意*ARM64 任务切换，所有必要的寄存器（x19-x28, fp, lr, sp）应该正确保存和恢复
**验证需求: 11.3**

**属性 49: ARM64 MMU 管理**
*对于任意*ARM64 页表操作，页表项应该正确设置访问权限和内存属性
**验证需求: 11.4**

**属性 50: ARM64 系统调用**
*对于任意*ARM64 系统调用，参数传递和返回值应该遵循 ARM64 调用约定
**验证需求: 11.7**

**属性 51: GIC-v3 初始化**
*对于任意*GIC-v3 硬件配置，初始化后 Distributor、Redistributor 和 CPU Interface 应该处于正确的工作状态
**验证需求: 11.8, 11.9**

**属性 52: GIC-v3 中断路由**
*对于任意*SPI 中断，设置路由后中断应该被发送到指定的 CPU 核心
**验证需求: 11.10**

**属性 53: GIC-v3 SGI 功能**
*对于任意*SGI 发送操作，目标 CPU 核心应该收到对应的软件中断
**验证需求: 11.11**

### 中断管理属性

**属性 51: 中断注册和注销**
*对于任意*中断号，注册后应该能够正确调用处理函数，注销后不应再调用
**验证需求: 12.1**

**属性 52: 中断优先级**
*对于任意*多个待处理的中断，应该按照优先级顺序处理
**验证需求: 12.2**

**属性 53: 中断嵌套**
*对于任意*中断嵌套配置，高优先级中断应该能够打断低优先级中断处理
**验证需求: 12.3**

**属性 54: 中断使能控制**
*对于任意*中断，禁用后不应触发处理，启用后应该能够正常触发
**验证需求: 12.4**

**属性 55: 共享中断处理**
*对于任意*共享中断，所有注册的处理函数都应该被调用
**验证需求: 12.5**

**属性 56: 中断统计准确性**
*对于任意*中断，统计信息（中断次数、处理时间）应该准确反映实际情况
**验证需求: 12.6**

**属性 57: 软中断和 Tasklet**
*对于任意*软中断或 tasklet，应该在中断上下文之外的安全时机执行
**验证需求: 12.7**

**属性 58: 中断抽象层**
*对于任意*中断管理操作，内核核心代码应该只使用架构无关的抽象接口
**验证需求: 12.8**

### 设备驱动模型属性

**属性 59: 设备注册和注销**
*对于任意*设备，注册后应该在设备列表中可见，注销后应该从列表中移除
**验证需求: 13.1**

**属性 60: 设备驱动自动匹配**
*对于任意*设备和驱动，如果 compatible 字符串匹配，应该自动调用 probe 函数
**验证需求: 13.2**

**属性 61: 设备类型分类**
*对于任意*设备，应该根据其类型正确分类到字符设备、块设备或网络设备
**验证需求: 13.3**

**属性 62: 设备树驱动探测**
*对于任意*设备树中定义的设备，应该自动探测并绑定匹配的驱动
**验证需求: 13.4**

**属性 63: 设备电源管理**
*对于任意*设备，电源管理操作应该正确改变设备的电源状态
**验证需求: 13.5**

**属性 64: 热插拔设备**
*对于任意*热插拔设备，插入时应该自动探测和初始化，拔出时应该正确清理
**验证需求: 13.6**

**属性 65: 总线抽象**
*对于任意*总线类型（platform, I2C, SPI），设备应该正确注册到对应的总线
**验证需求: 13.7**

**属性 66: Sysfs 设备信息**
*对于任意*注册的设备，应该在 sysfs 中导出设备信息
**验证需求: 13.8**

### 内存管理属性

**属性 67: Buddy 分配和释放**
*对于任意*页分配请求，分配后释放应该能够正确合并伙伴块
**验证需求: 14.1, 14.7**

**属性 68: Slab 对象分配**
*对于任意*Slab 缓存，分配的对象应该正确对齐并初始化
**验证需求: 14.2**

**属性 69: kmalloc/kfree 配对**
*对于任意*通过 kmalloc 分配的内存，kfree 后不应再被访问
**验证需求: 14.3**

**属性 70: 内存分配标志**
*对于任意*内存分配请求，应该根据 GFP 标志选择合适的分配策略
**验证需求: 14.5**

**属性 71: 页帧引用计数**
*对于任意*页帧，引用计数应该正确反映使用该页的数量
**验证需求: 14.6**

**属性 72: 虚拟内存分配**
*对于任意*vmalloc 分配的内存，应该在虚拟地址空间中连续但物理地址可以不连续
**验证需求: 14.4**

### 同步原语属性

**属性 73: 自旋锁互斥性**
*对于任意*自旋锁，同一时刻只能有一个持有者
**验证需求: 15.1**

**属性 74: 读写锁多读者**
*对于任意*读写锁，多个读者可以同时持有读锁，但写者独占
**验证需求: 15.2**

**属性 75: 互斥锁睡眠**
*对于任意*互斥锁，等待时任务应该进入睡眠状态而不是自旋
**验证需求: 15.3**

**属性 76: 信号量计数**
*对于任意*信号量，wait 和 post 操作应该正确维护资源计数
**验证需求: 15.4**

**属性 77: 完成量通知**
*对于任意*完成量，complete 操作应该唤醒所有等待的任务
**验证需求: 15.5**

**属性 78: 等待队列唤醒**
*对于任意*等待队列，wake_up 应该唤醒队列中的任务
**验证需求: 15.7**

**属性 79: 自旋锁中断安全**
*对于任意*使用 spin_lock_irqsave 的临界区，中断应该被正确禁用和恢复
**验证需求: 15.8**

**属性 80: 内存屏障顺序**
*对于任意*使用内存屏障的代码，内存访问应该按照屏障定义的顺序执行
**验证需求: 15.10**

### 原子操作属性

**属性 81: 原子操作不可分割**
*对于任意*原子操作，在多核环境下应该作为单个不可分割的操作执行
**验证需求: 16.6**

**属性 82: 原子比较交换**
*对于任意*atomic_cmpxchg 操作，只有当前值等于期望值时才应该更新
**验证需求: 16.3**

**属性 83: 原子算术正确性**
*对于任意*原子算术操作，结果应该与非原子版本在单线程下的结果一致
**验证需求: 16.2**

**属性 84: 原子位操作**
*对于任意*原子位操作，应该只影响指定的位而不影响其他位
**验证需求: 16.5**


## 错误处理

### 1. 错误码定义

**头文件: include/kernel/errno.h**
```c
/* 标准错误码 */
#define EPERM           1   /* 操作不允许 */
#define ENOENT          2   /* 文件或目录不存在 */
#define ESRCH           3   /* 进程不存在 */
#define EINTR           4   /* 系统调用被中断 */
#define EIO             5   /* I/O 错误 */
#define ENXIO           6   /* 设备或地址不存在 */
#define E2BIG           7   /* 参数列表过长 */
#define ENOEXEC         8   /* 执行格式错误 */
#define EBADF           9   /* 文件描述符错误 */
#define ECHILD          10  /* 没有子进程 */
#define EAGAIN          11  /* 资源暂时不可用 */
#define ENOMEM          12  /* 内存不足 */
#define EACCES          13  /* 权限被拒绝 */
#define EFAULT          14  /* 地址错误 */
#define EBUSY           16  /* 设备或资源忙 */
#define EEXIST          17  /* 文件已存在 */
#define ENODEV          19  /* 设备不存在 */
#define ENOTDIR         20  /* 不是目录 */
#define EISDIR          21  /* 是目录 */
#define EINVAL          22  /* 无效参数 */
#define ENFILE          23  /* 系统打开文件过多 */
#define EMFILE          24  /* 进程打开文件过多 */
#define ENOSPC          28  /* 设备空间不足 */
#define EROFS           30  /* 只读文件系统 */
#define ENOSYS          38  /* 功能未实现 */
#define ENOTEMPTY       39  /* 目录非空 */
#define ETIMEDOUT       110 /* 连接超时 */
```

### 2. 错误处理策略

#### 2.1 系统调用错误处理

```c
/* 系统调用返回值约定 */
- 成功: 返回 0 或正值（如文件描述符、读取字节数）
- 失败: 返回负的错误码（-EINVAL, -ENOMEM 等）

/* 示例 */
int sys_open(const char *pathname, int flags, mode_t mode)
{
    if (pathname == NULL)
        return -EFAULT;
    
    if (!valid_flags(flags))
        return -EINVAL;
    
    struct file *file = do_open(pathname, flags, mode);
    if (file == NULL)
        return -ENOENT;
    
    int fd = allocate_fd(file);
    if (fd < 0)
        return -EMFILE;
    
    return fd;
}
```

#### 2.2 内核内部错误处理

```c
/* 内核函数返回值约定 */
- 成功: 返回 0
- 失败: 返回负的错误码

/* 错误传播 */
int function_a(void)
{
    int ret = function_b();
    if (ret < 0) {
        pr_err("function_b failed: %d\n", ret);
        return ret;  /* 传播错误 */
    }
    return 0;
}
```

#### 2.3 致命错误处理

```c
/* BUG_ON 宏 - 用于不可恢复的错误 */
#define BUG_ON(condition) \
    do { \
        if (unlikely(condition)) { \
            pr_emerg("BUG: %s:%d %s\n", __FILE__, __LINE__, #condition); \
            panic("BUG detected"); \
        } \
    } while (0)

/* WARN_ON 宏 - 用于警告但可继续 */
#define WARN_ON(condition) \
    ({ \
        int __ret = !!(condition); \
        if (unlikely(__ret)) \
            pr_warning("WARNING: %s:%d %s\n", __FILE__, __LINE__, #condition); \
        __ret; \
    })

/* 使用示例 */
void critical_function(void *ptr)
{
    BUG_ON(ptr == NULL);  /* 空指针是致命错误 */
    
    if (WARN_ON(ptr->magic != MAGIC_VALUE)) {
        /* 魔数错误，记录警告但尝试继续 */
        return;
    }
}
```

### 3. 异常处理

#### 3.1 页错误处理

```c
/* 页错误处理流程 */
void do_page_fault(unsigned long addr, unsigned long error_code)
{
    struct task_struct *task = current;
    struct mm_struct *mm = task->mm;
    
    /* 1. 检查地址是否在有效范围内 */
    if (addr >= KERNEL_BASE) {
        /* 内核空间页错误 */
        if (error_code & PF_USER) {
            /* 用户态访问内核空间 */
            send_signal(task, SIGSEGV);
            return;
        }
        /* 内核态页错误 - 可能是 bug */
        panic("Kernel page fault at 0x%lx", addr);
    }
    
    /* 2. 查找 VMA */
    struct vm_area_struct *vma = find_vma(mm, addr);
    if (vma == NULL || addr < vma->vm_start) {
        /* 地址不在任何 VMA 中 */
        send_signal(task, SIGSEGV);
        return;
    }
    
    /* 3. 检查访问权限 */
    if ((error_code & PF_WRITE) && !(vma->vm_flags & VM_WRITE)) {
        /* 写只读页 */
        send_signal(task, SIGSEGV);
        return;
    }
    
    /* 4. 分配物理页并建立映射 */
    int ret = handle_mm_fault(mm, vma, addr, error_code);
    if (ret < 0) {
        /* 内存不足或其他错误 */
        send_signal(task, SIGBUS);
        return;
    }
}
```

#### 3.2 未定义指令处理

```c
void do_undefined_instruction(struct pt_regs *regs)
{
    unsigned long pc = regs->pc;
    unsigned int instr;
    
    /* 读取指令 */
    if (get_user(instr, (unsigned int *)pc) < 0) {
        /* 无法读取指令 */
        goto bad_area;
    }
    
    /* 尝试模拟指令 */
    if (emulate_instruction(instr, regs) == 0) {
        /* 模拟成功 */
        return;
    }
    
bad_area:
    /* 无法处理，发送信号 */
    pr_err("Undefined instruction at PC=0x%lx\n", pc);
    send_signal(current, SIGILL);
}
```

### 4. 资源清理

#### 4.1 错误路径清理模式

```c
/* goto 清理模式 */
int complex_operation(void)
{
    void *buffer = NULL;
    struct file *file = NULL;
    int ret = 0;
    
    buffer = kmalloc(SIZE, GFP_KERNEL);
    if (buffer == NULL) {
        ret = -ENOMEM;
        goto out;
    }
    
    file = file_open(PATH, O_RDWR);
    if (file == NULL) {
        ret = -ENOENT;
        goto out_free_buffer;
    }
    
    ret = do_work(buffer, file);
    if (ret < 0)
        goto out_close_file;
    
    /* 成功路径 */
    file_close(file);
    kfree(buffer);
    return 0;
    
out_close_file:
    file_close(file);
out_free_buffer:
    kfree(buffer);
out:
    return ret;
}
```

#### 4.2 RAII 风格清理（使用 cleanup 属性）

```c
/* 使用 GCC cleanup 属性 */
#define __cleanup(func) __attribute__((__cleanup__(func)))

static inline void cleanup_kfree(void *p)
{
    kfree(*(void **)p);
}

static inline void cleanup_file_close(struct file **f)
{
    if (*f)
        file_close(*f);
}

int complex_operation_v2(void)
{
    __cleanup(cleanup_kfree) void *buffer = kmalloc(SIZE, GFP_KERNEL);
    if (buffer == NULL)
        return -ENOMEM;
    
    __cleanup(cleanup_file_close) struct file *file = file_open(PATH, O_RDWR);
    if (file == NULL)
        return -ENOENT;
    
    return do_work(buffer, file);
    /* 自动清理 */
}
```

### 5. 调试支持

#### 5.1 断言和调试宏

```c
/* 断言宏 */
#ifdef CONFIG_DEBUG
#define ASSERT(condition) \
    do { \
        if (!(condition)) { \
            pr_emerg("Assertion failed: %s\n", #condition); \
            pr_emerg("  at %s:%d in %s\n", __FILE__, __LINE__, __func__); \
            BUG(); \
        } \
    } while (0)
#else
#define ASSERT(condition) do { } while (0)
#endif

/* 调试打印 */
#ifdef CONFIG_DEBUG
#define DEBUG(fmt, ...) pr_debug("%s: " fmt, __func__, ##__VA_ARGS__)
#else
#define DEBUG(fmt, ...) do { } while (0)
#endif
```

#### 5.2 内核 Panic

```c
void panic(const char *fmt, ...)
{
    va_list args;
    
    /* 禁用中断 */
    local_irq_disable();
    
    /* 打印 panic 消息 */
    pr_emerg("Kernel panic - not syncing: ");
    va_start(args, fmt);
    vprintk(fmt, args);
    va_end(args);
    pr_emerg("\n");
    
    /* 打印调用栈 */
    dump_stack();
    
    /* 停止系统 */
    while (1) {
        cpu_relax();
    }
}
```


## 测试策略

### 测试方法概述

本项目采用双重测试方法，结合单元测试和基于属性的测试（Property-Based Testing, PBT）来确保系统的正确性和可靠性。

- **单元测试**: 验证特定示例、边界情况和错误条件
- **属性测试**: 通过随机化验证所有输入的通用属性
- 两者互补，共同提供全面的测试覆盖

### 1. 单元测试策略

#### 1.1 测试框架选择

使用轻量级的嵌入式测试框架，适合内核环境：
- **Unity**: C 语言单元测试框架，适合嵌入式系统
- **自定义测试框架**: 基于内核的 printk 和断言机制

#### 1.2 单元测试范围

**构建系统测试:**
- 测试 Kconfig 配置生成
- 测试编译器切换
- 测试条件编译

**内存管理测试:**
- 测试内存分配和释放
- 测试 MMU 页表操作
- 测试内存泄漏检测

**VFS 测试:**
- 测试文件操作（open, read, write, close）
- 测试目录操作（mkdir, rmdir, chdir）
- 测试路径解析

**调度器测试:**
- 测试任务创建和销毁
- 测试优先级调度
- 测试上下文切换

**设备驱动测试:**
- 测试设备注册和注销
- 测试设备树解析
- 测试驱动匹配

#### 1.3 单元测试示例

```c
/* 测试文件: tests/test_vfs.c */
#include "unity.h"
#include <kernel/vfs.h>

void test_file_open_close(void)
{
    int fd = sys_open("/test.txt", O_RDWR | O_CREAT, 0644);
    TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
    
    int ret = sys_close(fd);
    TEST_ASSERT_EQUAL(0, ret);
}

void test_file_read_write(void)
{
    const char *data = "Hello, World!";
    char buffer[32];
    
    int fd = sys_open("/test.txt", O_RDWR | O_CREAT, 0644);
    TEST_ASSERT_GREATER_OR_EQUAL(0, fd);
    
    ssize_t written = sys_write(fd, data, strlen(data));
    TEST_ASSERT_EQUAL(strlen(data), written);
    
    sys_lseek(fd, 0, SEEK_SET);
    
    ssize_t read_bytes = sys_read(fd, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL(strlen(data), read_bytes);
    TEST_ASSERT_EQUAL_STRING(data, buffer);
    
    sys_close(fd);
}

void test_invalid_fd(void)
{
    char buffer[32];
    ssize_t ret = sys_read(999, buffer, sizeof(buffer));
    TEST_ASSERT_EQUAL(-EBADF, ret);
}
```

### 2. 基于属性的测试策略

#### 2.1 属性测试框架

由于这是 C 语言项目，我们将使用：
- **Theft**: C 语言的属性测试库
- **自定义生成器**: 为内核数据结构编写自定义的随机数据生成器

#### 2.2 属性测试配置

- **最小迭代次数**: 每个属性测试至少运行 100 次
- **标签格式**: `Feature: kernel-architecture-refactor, Property N: [属性描述]`
- **每个正确性属性对应一个属性测试**

#### 2.3 属性测试示例

**属性 31: 路径解析正确性**

```c
/* 测试文件: tests/property_test_vfs.c */
#include <theft.h>
#include <kernel/vfs.h>

/* Feature: kernel-architecture-refactor, Property 31: 路径解析正确性 */

/* 生成随机路径 */
static enum theft_alloc_res alloc_path(struct theft *t, void *env, void **output)
{
    const char *components[] = {"home", "usr", "bin", "etc", "tmp", "var"};
    int depth = theft_random_choice(t, 5) + 1;
    
    char *path = malloc(256);
    if (path == NULL)
        return THEFT_ALLOC_ERROR;
    
    path[0] = '/';
    path[1] = '\0';
    
    for (int i = 0; i < depth; i++) {
        int idx = theft_random_choice(t, sizeof(components) / sizeof(components[0]));
        strcat(path, components[idx]);
        if (i < depth - 1)
            strcat(path, "/");
    }
    
    *output = path;
    return THEFT_ALLOC_OK;
}

/* 属性: 对于任意有效路径，解析应该成功并返回正确的 inode */
static enum theft_trial_res prop_path_resolution(struct theft *t, void *arg1)
{
    char *path = (char *)arg1;
    
    /* 创建路径中的所有目录 */
    char *p = path + 1;
    while (*p) {
        if (*p == '/') {
            *p = '\0';
            sys_mkdir(path, 0755);
            *p = '/';
        }
        p++;
    }
    sys_mkdir(path, 0755);
    
    /* 解析路径 */
    struct dentry *dentry = path_lookup(path);
    if (dentry == NULL)
        return THEFT_TRIAL_FAIL;
    
    /* 验证路径名匹配 */
    if (strcmp(dentry->d_name, strrchr(path, '/') + 1) != 0)
        return THEFT_TRIAL_FAIL;
    
    return THEFT_TRIAL_PASS;
}

void test_property_path_resolution(void)
{
    struct theft_run_config config = {
        .name = "path_resolution",
        .prop1 = prop_path_resolution,
        .type_info = { { .alloc = alloc_path, .free = free } },
        .trials = 100,
    };
    
    enum theft_run_res res = theft_run(&config);
    TEST_ASSERT_EQUAL(THEFT_RUN_PASS, res);
}
```

**属性 40: 优先级调度顺序**

```c
/* Feature: kernel-architecture-refactor, Property 40: 优先级调度顺序 */

/* 生成随机任务集 */
static enum theft_alloc_res alloc_task_set(struct theft *t, void *env, void **output)
{
    int count = theft_random_choice(t, 10) + 2;  /* 2-11 个任务 */
    
    struct task_set {
        int count;
        struct task_info {
            int priority;
            struct task_struct *task;
        } tasks[11];
    } *set = malloc(sizeof(*set));
    
    if (set == NULL)
        return THEFT_ALLOC_ERROR;
    
    set->count = count;
    for (int i = 0; i < count; i++) {
        set->tasks[i].priority = theft_random_choice(t, 32);  /* 0-31 */
        set->tasks[i].task = NULL;
    }
    
    *output = set;
    return THEFT_ALLOC_OK;
}

/* 属性: 对于任意就绪任务集合，调度器应该选择优先级最高的任务 */
static enum theft_trial_res prop_priority_scheduling(struct theft *t, void *arg1)
{
    struct task_set *set = (struct task_set *)arg1;
    
    /* 创建所有任务 */
    for (int i = 0; i < set->count; i++) {
        set->tasks[i].task = task_create("test", dummy_entry, NULL, 
                                        set->tasks[i].priority);
        if (set->tasks[i].task == NULL)
            return THEFT_TRIAL_ERROR;
    }
    
    /* 找出最高优先级 */
    int max_priority = -1;
    for (int i = 0; i < set->count; i++) {
        if (set->tasks[i].priority > max_priority)
            max_priority = set->tasks[i].priority;
    }
    
    /* 触发调度 */
    schedule();
    
    /* 验证当前运行的任务是最高优先级的 */
    struct task_struct *current_task = get_current_task();
    int current_priority = get_task_priority(current_task);
    
    /* 清理 */
    for (int i = 0; i < set->count; i++) {
        if (set->tasks[i].task)
            task_destroy(set->tasks[i].task);
    }
    
    if (current_priority != max_priority)
        return THEFT_TRIAL_FAIL;
    
    return THEFT_TRIAL_PASS;
}
```

**属性 27: 字符串函数功能等效性**

```c
/* Feature: kernel-architecture-refactor, Property 27: 字符串函数功能等效性 */

/* 生成随机字符串 */
static enum theft_alloc_res alloc_string(struct theft *t, void *env, void **output)
{
    size_t len = theft_random_choice(t, 100) + 1;
    char *str = malloc(len + 1);
    if (str == NULL)
        return THEFT_ALLOC_ERROR;
    
    for (size_t i = 0; i < len; i++) {
        str[i] = 'a' + theft_random_choice(t, 26);
    }
    str[len] = '\0';
    
    *output = str;
    return THEFT_ALLOC_OK;
}

/* 属性: 内核 strlen 应该与标准 strlen 行为一致 */
static enum theft_trial_res prop_strlen_equivalence(struct theft *t, void *arg1)
{
    char *str = (char *)arg1;
    
    size_t kernel_len = kernel_strlen(str);
    size_t std_len = strlen(str);
    
    if (kernel_len != std_len)
        return THEFT_TRIAL_FAIL;
    
    return THEFT_TRIAL_PASS;
}

/* 属性: 内核 strcmp 应该与标准 strcmp 行为一致 */
static enum theft_trial_res prop_strcmp_equivalence(struct theft *t, void *arg1, void *arg2)
{
    char *str1 = (char *)arg1;
    char *str2 = (char *)arg2;
    
    int kernel_cmp = kernel_strcmp(str1, str2);
    int std_cmp = strcmp(str1, str2);
    
    /* 比较符号（正、负、零） */
    if ((kernel_cmp > 0) != (std_cmp > 0))
        return THEFT_TRIAL_FAIL;
    if ((kernel_cmp < 0) != (std_cmp < 0))
        return THEFT_TRIAL_FAIL;
    if ((kernel_cmp == 0) != (std_cmp == 0))
        return THEFT_TRIAL_FAIL;
    
    return THEFT_TRIAL_PASS;
}
```

### 3. 集成测试

#### 3.1 QEMU 测试环境

**重要**: 重构后的内核必须能够在 QEMU 上成功运行和测试。QEMU 是主要的测试和验证平台。

**QEMU ARM 测试配置:**

```bash
# 启动 QEMU ARM (Cortex-A15)
qemu-system-arm \
    -M virt \
    -cpu cortex-a15 \
    -m 512M \
    -kernel nos-arm.elf \
    -dtb board/qemu-virt/qemu-virt-arm.dtb \
    -nographic \
    -serial mon:stdio \
    -append "console=ttyAMA0"
```

**QEMU ARM64 测试配置:**

```bash
# 启动 QEMU ARM64 (Cortex-A53)
qemu-system-aarch64 \
    -M virt \
    -cpu cortex-a53 \
    -m 512M \
    -kernel nos-arm64.elf \
    -dtb board/qemu-virt/qemu-virt-arm64.dtb \
    -nographic \
    -serial mon:stdio \
    -append "console=ttyAMA0"
```

**QEMU 测试脚本:**

```bash
#!/bin/bash
# scripts/qemu_test.sh

set -e

ARCH=${1:-arm}
TIMEOUT=30

if [ "$ARCH" = "arm" ]; then
    QEMU=qemu-system-arm
    CPU=cortex-a15
    KERNEL=nos-arm.elf
    DTB=board/qemu-virt/qemu-virt-arm.dtb
elif [ "$ARCH" = "arm64" ]; then
    QEMU=qemu-system-aarch64
    CPU=cortex-a53
    KERNEL=nos-arm64.elf
    DTB=board/qemu-virt/qemu-virt-arm64.dtb
else
    echo "Unknown architecture: $ARCH"
    exit 1
fi

echo "Starting QEMU test for $ARCH..."

# 启动 QEMU 并捕获输出
timeout $TIMEOUT $QEMU \
    -M virt \
    -cpu $CPU \
    -m 512M \
    -kernel $KERNEL \
    -dtb $DTB \
    -nographic \
    -serial mon:stdio \
    -append "console=ttyAMA0 test=1" \
    > qemu_output.log 2>&1 || true

# 检查测试结果
if grep -q "All tests passed" qemu_output.log; then
    echo "✓ QEMU test passed for $ARCH"
    exit 0
else
    echo "✗ QEMU test failed for $ARCH"
    cat qemu_output.log
    exit 1
fi
```

**QEMU 调试配置:**

```bash
# 启动 QEMU 并等待 GDB 连接
qemu-system-aarch64 \
    -M virt \
    -cpu cortex-a53 \
    -m 512M \
    -kernel nos-arm64.elf \
    -dtb board/qemu-virt/qemu-virt-arm64.dtb \
    -nographic \
    -serial mon:stdio \
    -s -S  # 等待 GDB 连接到 localhost:1234

# 在另一个终端启动 GDB
aarch64-none-elf-gdb nos-arm64.elf \
    -ex "target remote localhost:1234" \
    -ex "break nos_start" \
    -ex "continue"
```

#### 3.2 QEMU 自动化测试套件

**测试内核模块 (kernel/test/qemu_test.c):**

```c
/* QEMU 自动化测试入口 */
void qemu_run_tests(void)
{
    int passed = 0;
    int failed = 0;
    
    pr_info("=== Starting QEMU Test Suite ===\n");
    
    /* 测试内存管理 */
    if (test_memory_management() == 0) {
        pr_info("✓ Memory management test passed\n");
        passed++;
    } else {
        pr_err("✗ Memory management test failed\n");
        failed++;
    }
    
    /* 测试 VFS */
    if (test_vfs() == 0) {
        pr_info("✓ VFS test passed\n");
        passed++;
    } else {
        pr_err("✗ VFS test failed\n");
        failed++;
    }
    
    /* 测试调度器 */
    if (test_scheduler() == 0) {
        pr_info("✓ Scheduler test passed\n");
        passed++;
    } else {
        pr_err("✗ Scheduler test failed\n");
        failed++;
    }
    
    /* 测试中断管理 */
    if (test_irq_management() == 0) {
        pr_info("✓ IRQ management test passed\n");
        passed++;
    } else {
        pr_err("✗ IRQ management test failed\n");
        failed++;
    }
    
    /* 测试设备驱动模型 */
    if (test_device_model() == 0) {
        pr_info("✓ Device model test passed\n");
        passed++;
    } else {
        pr_err("✗ Device model test failed\n");
        failed++;
    }
    
    /* 测试 GIC (ARM64) */
#ifdef CONFIG_ARM64
    if (test_gic_v3() == 0) {
        pr_info("✓ GIC-v3 test passed\n");
        passed++;
    } else {
        pr_err("✗ GIC-v3 test failed\n");
        failed++;
    }
#endif
    
    /* 打印测试结果 */
    pr_info("=== Test Results ===\n");
    pr_info("Passed: %d\n", passed);
    pr_info("Failed: %d\n", failed);
    
    if (failed == 0) {
        pr_info("All tests passed\n");
    } else {
        pr_err("Some tests failed\n");
    }
    
    /* 关闭 QEMU */
    qemu_exit(failed == 0 ? 0 : 1);
}

/* QEMU 退出函数 */
void qemu_exit(int code)
{
    /* 使用 QEMU semihosting 退出 */
#ifdef CONFIG_ARM64
    register long x0 asm("x0") = 0x18;  /* SYS_EXIT */
    register long x1 asm("x1") = code;
    asm volatile("hlt #0xF000" : : "r"(x0), "r"(x1));
#else
    register long r0 asm("r0") = 0x18;  /* SYS_EXIT */
    register long r1 asm("r1") = code;
    asm volatile("svc #0x123456" : : "r"(r0), "r"(r1));
#endif
    
    /* 如果 semihosting 不可用，进入死循环 */
    while (1)
        cpu_relax();
}
```

**Makefile 集成:**

```makefile
# Makefile

# QEMU 测试目标
qemu-test: $(KERNEL_ELF) $(DTB)
	@echo "Running QEMU test for $(ARCH)..."
	@./scripts/qemu_test.sh $(ARCH)

qemu-test-arm: arm_config
	@$(MAKE) clean
	@$(MAKE) -j$(nproc)
	@$(MAKE) qemu-test ARCH=arm

qemu-test-arm64: arm64_config
	@$(MAKE) clean
	@$(MAKE) -j$(nproc)
	@$(MAKE) qemu-test ARCH=arm64

qemu-test-all: qemu-test-arm qemu-test-arm64

# QEMU 调试目标
qemu-debug: $(KERNEL_ELF) $(DTB)
	@echo "Starting QEMU in debug mode..."
	@qemu-system-$(QEMU_ARCH) \
		-M virt \
		-cpu $(QEMU_CPU) \
		-m 512M \
		-kernel $(KERNEL_ELF) \
		-dtb $(DTB) \
		-nographic \
		-serial mon:stdio \
		-s -S

# QEMU 运行目标
qemu-run: $(KERNEL_ELF) $(DTB)
	@echo "Starting QEMU..."
	@qemu-system-$(QEMU_ARCH) \
		-M virt \
		-cpu $(QEMU_CPU) \
		-m 512M \
		-kernel $(KERNEL_ELF) \
		-dtb $(DTB) \
		-nographic \
		-serial mon:stdio
```

#### 3.2 自动化测试流程

```bash
#!/bin/bash
# scripts/run_tests.sh

# 1. 构建测试内核
make clean
make test_config
make -j$(nproc)

# 2. 运行单元测试
make unit-test

# 3. 运行属性测试
make property-test

# 4. 运行 QEMU 集成测试
make qemu-test

# 5. 生成测试报告
make test-report
```

### 4. 持续集成

#### 4.1 CI 配置

```yaml
# .gitlab-ci.yml
test:
  stage: test
  script:
    - make clean
    - make test_config
    - make -j$(nproc)
    - make unit-test
    - make property-test
    - make qemu-test
  artifacts:
    reports:
      junit: test-results.xml
    paths:
      - test-results/
```

### 5. 测试覆盖率

#### 5.1 代码覆盖率工具

使用 gcov/lcov 进行代码覆盖率分析：

```makefile
# Makefile 中添加覆盖率支持
ifeq ($(CONFIG_COVERAGE),y)
CFLAGS += -fprofile-arcs -ftest-coverage
LDFLAGS += -lgcov
endif

coverage:
	lcov --capture --directory . --output-file coverage.info
	genhtml coverage.info --output-directory coverage-report
```

#### 5.2 覆盖率目标

- **行覆盖率**: 目标 > 80%
- **分支覆盖率**: 目标 > 70%
- **函数覆盖率**: 目标 > 90%

### 6. 性能测试

#### 6.1 基准测试

```c
/* 调度器性能测试 */
void benchmark_scheduler(void)
{
    uint64_t start = get_cycles();
    
    for (int i = 0; i < 1000; i++) {
        schedule();
    }
    
    uint64_t end = get_cycles();
    uint64_t avg = (end - start) / 1000;
    
    pr_info("Average schedule time: %llu cycles\n", avg);
}

/* VFS 性能测试 */
void benchmark_vfs(void)
{
    uint64_t start = get_cycles();
    
    for (int i = 0; i < 1000; i++) {
        int fd = sys_open("/test.txt", O_RDWR);
        sys_close(fd);
    }
    
    uint64_t end = get_cycles();
    uint64_t avg = (end - start) / 1000;
    
    pr_info("Average open/close time: %llu cycles\n", avg);
}
```

### 7. 测试最佳实践

#### 7.1 单元测试原则

- 每个测试应该独立，不依赖其他测试
- 测试应该快速执行
- 测试应该可重复
- 使用描述性的测试名称

#### 7.2 属性测试原则

- 避免过多的单元测试 - 属性测试可以覆盖大量输入
- 单元测试专注于：
  - 具体示例
  - 集成点
  - 边界情况和错误条件
- 属性测试专注于：
  - 对所有输入都成立的通用属性
  - 通过随机化实现全面的输入覆盖

#### 7.3 测试数据生成

- 编写智能的生成器，合理约束输入空间
- 生成器应该能够生成有效和无效的输入
- 使用种子确保测试可重现

## 实施计划概述

本重构项目将分阶段实施，每个阶段都有明确的目标和可交付成果：

### 阶段 1: 构建系统重构
- 引入 Kconfig 配置系统
- 支持 Clang 编译器
- 重组 Makefile 结构

### 阶段 2: 目录结构重组
- 重构 arch 目录
- 精简 board 目录
- 移动驱动代码到 drivers

### 阶段 3: 架构抽象层
- 定义架构抽象接口
- 实现 ARM 架构支持
- 实现 ARM64 架构支持

### 阶段 4: 核心子系统完善
- 完善 VFS
- 完善调度器
- 完善中断管理
- 完善设备驱动模型

### 阶段 5: 高级特性
- MMU 支持
- 设备树支持
- Shell 系统
- 独立 C 运行时

### 阶段 6: 测试和验证
- 单元测试
- 属性测试
- 集成测试
- 性能测试

每个阶段都将包含相应的测试，确保重构不会破坏现有功能。

## 补充：核心子系统详细设计

### 1. 内存管理子系统

#### 1.1 内存分配器架构

**分层设计:**
```
┌─────────────────────────────────────┐
│  高层接口 (kmalloc, kfree)          │
└─────────────────────────────────────┘
                ↓
┌─────────────────────────────────────┐
│  Slab 分配器 (小对象缓存)           │
└─────────────────────────────────────┘
                ↓
┌─────────────────────────────────────┐
│  Buddy 分配器 (页级分配)            │
└─────────────────────────────────────┘
                ↓
┌─────────────────────────────────────┐
│  物理内存管理 (页帧管理)            │
└─────────────────────────────────────┘
```

#### 1.2 Buddy 系统

**头文件: include/kernel/mm/buddy.h**
```c
/* Buddy 系统配置 */
#define MAX_ORDER 11  /* 最大阶数，支持 2^11 = 2048 页 */
#define PAGE_SHIFT 12 /* 4KB 页 */
#define PAGE_SIZE (1UL << PAGE_SHIFT)

/* 页帧描述符 */
struct page {
    unsigned long flags;        /* 页标志 */
    atomic_t refcount;         /* 引用计数 */
    struct list_head lru;      /* LRU 链表 */
    void *virtual;             /* 虚拟地址 */
    unsigned int order;        /* Buddy 阶数 */
};

/* 页标志 */
#define PG_locked       0  /* 页被锁定 */
#define PG_reserved     1  /* 页被保留 */
#define PG_slab         2  /* 页属于 slab */
#define PG_dirty        3  /* 页已修改 */
#define PG_lru          4  /* 页在 LRU 链表中 */

/* Buddy 系统接口 */
struct page *alloc_pages(unsigned int order);
void free_pages(struct page *page, unsigned int order);
struct page *alloc_page(void);
void free_page(struct page *page);

/* 页帧号转换 */
unsigned long page_to_pfn(struct page *page);
struct page *pfn_to_page(unsigned long pfn);
void *page_address(struct page *page);
```

**实现文件: kernel/mm/buddy.c**
```c
/* Buddy 系统数据结构 */
struct free_area {
    struct list_head free_list;
    unsigned long nr_free;
};

struct zone {
    struct free_area free_area[MAX_ORDER];
    unsigned long managed_pages;
    unsigned long present_pages;
    spinlock_t lock;
};

static struct zone mem_zone;
static struct page *mem_map;  /* 页帧数组 */

/* 初始化 Buddy 系统 */
void buddy_init(unsigned long start_pfn, unsigned long end_pfn)
{
    unsigned long nr_pages = end_pfn - start_pfn;
    
    /* 分配页帧数组 */
    mem_map = (struct page *)early_alloc(nr_pages * sizeof(struct page));
    
    /* 初始化所有页帧 */
    for (unsigned long i = 0; i < nr_pages; i++) {
        struct page *page = &mem_map[i];
        atomic_set(&page->refcount, 0);
        page->flags = 0;
        page->order = 0;
        INIT_LIST_HEAD(&page->lru);
    }
    
    /* 初始化空闲链表 */
    for (int order = 0; order < MAX_ORDER; order++) {
        INIT_LIST_HEAD(&mem_zone.free_area[order].free_list);
        mem_zone.free_area[order].nr_free = 0;
    }
    
    spin_lock_init(&mem_zone.lock);
    
    /* 将所有页加入 Buddy 系统 */
    for (unsigned long pfn = start_pfn; pfn < end_pfn; ) {
        int order = MAX_ORDER - 1;
        
        /* 找到最大的对齐块 */
        while (order > 0) {
            if ((pfn & ((1UL << order) - 1)) == 0 &&
                pfn + (1UL << order) <= end_pfn)
                break;
            order--;
        }
        
        struct page *page = pfn_to_page(pfn);
        page->order = order;
        list_add(&page->lru, &mem_zone.free_area[order].free_list);
        mem_zone.free_area[order].nr_free++;
        
        pfn += (1UL << order);
    }
}

/* 分配页 */
struct page *alloc_pages(unsigned int order)
{
    unsigned long flags;
    struct page *page = NULL;
    
    spin_lock_irqsave(&mem_zone.lock, &flags);
    
    /* 从请求的阶数开始查找 */
    for (int current_order = order; current_order < MAX_ORDER; current_order++) {
        struct free_area *area = &mem_zone.free_area[current_order];
        
        if (list_empty(&area->free_list))
            continue;
        
        /* 找到空闲块 */
        page = list_first_entry(&area->free_list, struct page, lru);
        list_del(&page->lru);
        area->nr_free--;
        
        /* 分裂大块 */
        while (current_order > order) {
            current_order--;
            area--;
            
            /* 将伙伴块加入低一阶的空闲链表 */
            struct page *buddy = page + (1UL << current_order);
            buddy->order = current_order;
            list_add(&buddy->lru, &area->free_list);
            area->nr_free++;
        }
        
        page->order = order;
        atomic_set(&page->refcount, 1);
        break;
    }
    
    spin_unlock_irqrestore(&mem_zone.lock, flags);
    
    return page;
}

/* 释放页 */
void free_pages(struct page *page, unsigned int order)
{
    unsigned long flags;
    unsigned long pfn = page_to_pfn(page);
    
    spin_lock_irqsave(&mem_zone.lock, &flags);
    
    /* 合并伙伴块 */
    while (order < MAX_ORDER - 1) {
        unsigned long buddy_pfn = pfn ^ (1UL << order);
        struct page *buddy = pfn_to_page(buddy_pfn);
        
        /* 检查伙伴块是否空闲且阶数相同 */
        if (atomic_read(&buddy->refcount) != 0 || buddy->order != order)
            break;
        
        /* 从空闲链表中移除伙伴块 */
        list_del(&buddy->lru);
        mem_zone.free_area[order].nr_free--;
        
        /* 合并 */
        if (buddy_pfn < pfn)
            page = buddy;
        
        pfn &= ~(1UL << order);
        order++;
    }
    
    /* 将合并后的块加入空闲链表 */
    page->order = order;
    atomic_set(&page->refcount, 0);
    list_add(&page->lru, &mem_zone.free_area[order].free_list);
    mem_zone.free_area[order].nr_free++;
    
    spin_unlock_irqrestore(&mem_zone.lock, flags);
}
```

#### 1.3 Slab 分配器

**头文件: include/kernel/mm/slab.h**
```c
/* Slab 缓存 */
struct kmem_cache {
    const char *name;
    size_t size;
    size_t align;
    unsigned long flags;
    void (*ctor)(void *);
    
    struct list_head slabs_full;
    struct list_head slabs_partial;
    struct list_head slabs_free;
    
    spinlock_t lock;
    unsigned int num_objs;
    unsigned int num_active;
};

/* Slab 描述符 */
struct slab {
    struct list_head list;
    void *s_mem;
    unsigned int inuse;
    unsigned int free;
    void *freelist;
};

/* Slab 接口 */
struct kmem_cache *kmem_cache_create(const char *name, size_t size,
                                     size_t align, unsigned long flags,
                                     void (*ctor)(void *));
void kmem_cache_destroy(struct kmem_cache *cache);
void *kmem_cache_alloc(struct kmem_cache *cache);
void kmem_cache_free(struct kmem_cache *cache, void *obj);

/* 通用内存分配 */
void *kmalloc(size_t size, gfp_t flags);
void kfree(const void *ptr);
void *kzalloc(size_t size, gfp_t flags);
void *krealloc(const void *ptr, size_t new_size, gfp_t flags);

/* 内存分配标志 */
#define GFP_KERNEL  0x00  /* 可睡眠 */
#define GFP_ATOMIC  0x01  /* 不可睡眠 */
#define GFP_DMA     0x02  /* DMA 内存 */
#define GFP_ZERO    0x04  /* 清零 */
```

#### 1.4 虚拟内存管理 (CONFIG_MMU=y)

**头文件: include/kernel/mm/vmalloc.h**
```c
/* 虚拟内存区域 */
struct vm_area_struct {
    unsigned long vm_start;
    unsigned long vm_end;
    unsigned long vm_flags;
    struct mm_struct *vm_mm;
    struct vm_area_struct *vm_next;
    struct file *vm_file;
    unsigned long vm_pgoff;
};

/* VMA 标志 */
#define VM_READ     0x00000001
#define VM_WRITE    0x00000002
#define VM_EXEC     0x00000004
#define VM_SHARED   0x00000008
#define VM_IO       0x00000010
#define VM_DONTCOPY 0x00000020

/* 内存描述符 */
struct mm_struct {
    struct vm_area_struct *mmap;
    pgd_t *pgd;
    atomic_t mm_users;
    atomic_t mm_count;
    unsigned long start_code;
    unsigned long end_code;
    unsigned long start_data;
    unsigned long end_data;
    unsigned long start_brk;
    unsigned long brk;
    unsigned long start_stack;
    spinlock_t page_table_lock;
};

/* 虚拟内存接口 */
void *vmalloc(unsigned long size);
void vfree(const void *addr);
int remap_pfn_range(struct vm_area_struct *vma, unsigned long addr,
                    unsigned long pfn, unsigned long size, pgprot_t prot);
```

### 2. 同步原语详细实现

#### 2.1 自旋锁 (Spinlock)

**实现文件: kernel/sync/spinlock.c**
```c
/* 自旋锁初始化 */
void spin_lock_init(spinlock_t *lock)
{
    atomic_set(&lock->lock, 0);
#ifdef CONFIG_DEBUG_SPINLOCK
    lock->owner = NULL;
    lock->file = NULL;
    lock->line = 0;
#endif
}

/* 获取自旋锁 */
void spin_lock(spinlock_t *lock)
{
    /* 使用 test-and-set 原子操作 */
    while (1) {
        /* 尝试获取锁 */
        if (atomic_cmpxchg(&lock->lock, 0, 1) == 0) {
#ifdef CONFIG_DEBUG_SPINLOCK
            lock->owner = current;
#endif
            /* 内存屏障，确保锁获取后的操作不会被重排到锁获取之前 */
            smp_mb();
            return;
        }
        
        /* 等待锁释放，使用 pause 指令减少总线流量 */
        while (atomic_read(&lock->lock) != 0)
            cpu_relax();
    }
}

/* 尝试获取自旋锁 */
int spin_trylock(spinlock_t *lock)
{
    if (atomic_cmpxchg(&lock->lock, 0, 1) == 0) {
#ifdef CONFIG_DEBUG_SPINLOCK
        lock->owner = current;
#endif
        smp_mb();
        return 1;
    }
    return 0;
}

/* 释放自旋锁 */
void spin_unlock(spinlock_t *lock)
{
#ifdef CONFIG_DEBUG_SPINLOCK
    BUG_ON(lock->owner != current);
    lock->owner = NULL;
#endif
    
    /* 内存屏障，确保锁保护的操作完成后才释放锁 */
    smp_mb();
    
    /* 释放锁 */
    atomic_set(&lock->lock, 0);
}

/* 自旋锁 + 禁用中断 */
void spin_lock_irq(spinlock_t *lock)
{
    arch_local_irq_disable();
    spin_lock(lock);
}

void spin_unlock_irq(spinlock_t *lock)
{
    spin_unlock(lock);
    arch_local_irq_enable();
}

void spin_lock_irqsave(spinlock_t *lock, unsigned long *flags)
{
    *flags = arch_local_irq_save();
    spin_lock(lock);
}

void spin_unlock_irqrestore(spinlock_t *lock, unsigned long flags)
{
    spin_unlock(lock);
    arch_local_irq_restore(flags);
}
```

#### 2.2 读写自旋锁 (RW Spinlock)

**实现文件: kernel/sync/rwlock.c**
```c
void read_lock(rwlock_t *lock)
{
    while (1) {
        /* 等待写锁释放 */
        while (atomic_read(&lock->lock) != 0)
            cpu_relax();
        
        /* 增加读者计数 */
        atomic_inc(&lock->readers);
        
        /* 检查是否有写者 */
        if (atomic_read(&lock->lock) == 0) {
            smp_mb();
            return;
        }
        
        /* 有写者，减少读者计数并重试 */
        atomic_dec(&lock->readers);
    }
}

void read_unlock(rwlock_t *lock)
{
    smp_mb();
    atomic_dec(&lock->readers);
}

void write_lock(rwlock_t *lock)
{
    /* 获取写锁 */
    while (atomic_cmpxchg(&lock->lock, 0, 1) != 0)
        cpu_relax();
    
    /* 等待所有读者退出 */
    while (atomic_read(&lock->readers) != 0)
        cpu_relax();
    
    smp_mb();
}

void write_unlock(rwlock_t *lock)
{
    smp_mb();
    atomic_set(&lock->lock, 0);
}
```

#### 2.3 互斥锁 (Mutex)

**实现文件: kernel/sync/mutex.c**
```c
void mutex_init(struct mutex *lock)
{
    atomic_set(&lock->locked, 0);
    lock->owner = NULL;
    INIT_LIST_HEAD(&lock->wait_list);
    spin_lock_init(&lock->wait_lock);
}

void mutex_lock(struct mutex *lock)
{
    /* 快速路径：尝试直接获取锁 */
    if (atomic_cmpxchg(&lock->locked, 0, 1) == 0) {
        lock->owner = current;
        smp_mb();
        return;
    }
    
    /* 慢速路径：需要等待 */
    unsigned long flags;
    spin_lock_irqsave(&lock->wait_lock, &flags);
    
    /* 再次尝试获取锁 */
    if (atomic_cmpxchg(&lock->locked, 0, 1) == 0) {
        lock->owner = current;
        spin_unlock_irqrestore(&lock->wait_lock, flags);
        smp_mb();
        return;
    }
    
    /* 将当前任务加入等待队列 */
    list_add_tail(&current->wait_list, &lock->wait_list);
    current->state = TASK_BLOCKED;
    
    spin_unlock_irqrestore(&lock->wait_lock, flags);
    
    /* 调度到其他任务 */
    schedule();
    
    /* 被唤醒后，锁已经被获取 */
    smp_mb();
}

int mutex_trylock(struct mutex *lock)
{
    if (atomic_cmpxchg(&lock->locked, 0, 1) == 0) {
        lock->owner = current;
        smp_mb();
        return 1;
    }
    return 0;
}

void mutex_unlock(struct mutex *lock)
{
    BUG_ON(lock->owner != current);
    
    unsigned long flags;
    spin_lock_irqsave(&lock->wait_lock, &flags);
    
    lock->owner = NULL;
    smp_mb();
    atomic_set(&lock->locked, 0);
    
    /* 唤醒等待队列中的第一个任务 */
    if (!list_empty(&lock->wait_list)) {
        struct task_struct *task = list_first_entry(&lock->wait_list,
                                                     struct task_struct,
                                                     wait_list);
        list_del(&task->wait_list);
        task->state = TASK_READY;
        
        /* 直接将锁转移给被唤醒的任务 */
        atomic_set(&lock->locked, 1);
        lock->owner = task;
        
        wake_up(task);
    }
    
    spin_unlock_irqrestore(&lock->wait_lock, flags);
}
```

#### 2.4 信号量 (Semaphore)

**实现文件: kernel/sync/semaphore.c**
```c
void sem_init(struct semaphore *sem, int value)
{
    atomic_set(&sem->count, value);
    INIT_LIST_HEAD(&sem->wait_list);
    spin_lock_init(&sem->wait_lock);
}

void sem_wait(struct semaphore *sem)
{
    /* 尝试减少计数 */
    if (atomic_dec_return(&sem->count) >= 0) {
        smp_mb();
        return;
    }
    
    /* 计数为负，需要等待 */
    unsigned long flags;
    spin_lock_irqsave(&sem->wait_lock, &flags);
    
    /* 将当前任务加入等待队列 */
    list_add_tail(&current->wait_list, &sem->wait_list);
    current->state = TASK_BLOCKED;
    
    spin_unlock_irqrestore(&sem->wait_lock, flags);
    
    /* 调度到其他任务 */
    schedule();
    
    smp_mb();
}

int sem_trywait(struct semaphore *sem)
{
    int old_count = atomic_read(&sem->count);
    
    if (old_count > 0) {
        if (atomic_cmpxchg(&sem->count, old_count, old_count - 1) == old_count) {
            smp_mb();
            return 0;
        }
    }
    
    return -EAGAIN;
}

void sem_post(struct semaphore *sem)
{
    unsigned long flags;
    spin_lock_irqsave(&sem->wait_lock, &flags);
    
    /* 增加计数 */
    atomic_inc(&sem->count);
    smp_mb();
    
    /* 唤醒等待队列中的第一个任务 */
    if (!list_empty(&sem->wait_list)) {
        struct task_struct *task = list_first_entry(&sem->wait_list,
                                                     struct task_struct,
                                                     wait_list);
        list_del(&task->wait_list);
        task->state = TASK_READY;
        wake_up(task);
    }
    
    spin_unlock_irqrestore(&sem->wait_lock, flags);
}
```

### 3. 内存屏障和原子操作

#### 3.1 内存屏障

**头文件: include/asm-generic/barrier.h**
```c
/* 编译器屏障 */
#define barrier() asm volatile("" ::: "memory")

/* 内存屏障 */
#ifdef CONFIG_ARM64
#define mb()    asm volatile("dmb sy" ::: "memory")
#define rmb()   asm volatile("dmb ld" ::: "memory")
#define wmb()   asm volatile("dmb st" ::: "memory")
#define smp_mb()  mb()
#define smp_rmb() rmb()
#define smp_wmb() wmb()
#else /* ARM */
#define mb()    asm volatile("dmb" ::: "memory")
#define rmb()   asm volatile("dmb" ::: "memory")
#define wmb()   asm volatile("dmb st" ::: "memory")
#define smp_mb()  mb()
#define smp_rmb() rmb()
#define smp_wmb() wmb()
#endif

/* CPU 放松指令 */
#ifdef CONFIG_ARM64
#define cpu_relax() asm volatile("yield" ::: "memory")
#else
#define cpu_relax() asm volatile("yield" ::: "memory")
#endif
```

#### 3.2 原子操作实现

**ARM64 原子操作 (arch/arm64/include/asm/atomic.h):**
```c
static inline int atomic_read(const atomic_t *v)
{
    return READ_ONCE(v->counter);
}

static inline void atomic_set(atomic_t *v, int i)
{
    WRITE_ONCE(v->counter, i);
}

static inline void atomic_add(int i, atomic_t *v)
{
    unsigned long tmp;
    int result;
    
    asm volatile(
    "1: ldxr    %w0, [%2]\n"
    "   add     %w0, %w0, %w3\n"
    "   stxr    %w1, %w0, [%2]\n"
    "   cbnz    %w1, 1b"
    : "=&r" (result), "=&r" (tmp), "+Q" (v->counter)
    : "Ir" (i)
    : "cc");
}

static inline int atomic_cmpxchg(atomic_t *v, int old, int new)
{
    unsigned long tmp;
    int oldval;
    
    asm volatile(
    "1: ldxr    %w0, [%2]\n"
    "   cmp     %w0, %w3\n"
    "   b.ne    2f\n"
    "   stxr    %w1, %w4, [%2]\n"
    "   cbnz    %w1, 1b\n"
    "2:"
    : "=&r" (oldval), "=&r" (tmp), "+Q" (v->counter)
    : "Ir" (old), "r" (new)
    : "cc");
    
    return oldval;
}

static inline int atomic_inc_return(atomic_t *v)
{
    unsigned long tmp;
    int result;
    
    asm volatile(
    "1: ldxr    %w0, [%2]\n"
    "   add     %w0, %w0, #1\n"
    "   stlxr   %w1, %w0, [%2]\n"
    "   cbnz    %w1, 1b"
    : "=&r" (result), "=&r" (tmp), "+Q" (v->counter)
    :
    : "cc", "memory");
    
    return result;
}
```

这些核心子系统的详细设计确保了内核具有完整的内存管理、同步机制和原子操作支持。
