# Kconfig 配置系统使用指南

## 概述

NOS 内核现在支持功能完整的 Kconfig 配置系统，类似于 Linux 内核的配置方式。这提供了强大的配置管理能力，包括依赖关系处理、互斥选项和层级菜单。

## 基本使用

### 1. 使用默认配置

```bash
make defconfig
```

这将使用架构的默认配置文件（`arch/<ARCH>/configs/defconfig`）。

### 2. 使用特定板级配置

```bash
make nk60-v2_config
```

这将使用板级特定的配置文件（`arch/arm/configs/nk60-v2_defconfig`）。

支持的板级配置：
- `nk60-v2_config` - NK60 V2 键盘板
- `qemu-stm32_config` - QEMU STM32 仿真板
- `wl-001_config` - WL-001 板
- `zj-v3_config` - ZJ V3 板

### 3. 交互式配置（menuconfig）

```bash
make menuconfig
```

这将启动一个功能完整的交互式界面，支持：

**层级菜单导航:**
- 数字键：选择菜单项
- 'b'：返回上级菜单
- 's'：保存配置
- 'q'：退出

**配置选项类型:**
- **布尔选项**: 空格键切换 `[ ]` / `[*]`
- **三态选项**: 空格键循环 `[ ]` / `[*]` / `[M]`
- **字符串/数值**: 直接输入编辑
- **选择组**: 进入子菜单选择互斥选项

**智能功能:**
- 自动依赖关系检查
- 互斥选项处理
- 配置验证
- 实时可见性更新

### 4. 保存最小配置

```bash
make savedefconfig
```

这将保存一个最小化的配置文件，只包含与默认值不同的选项。

## 配置文件位置

- **Kconfig 文件**：定义配置选项
  - `Kconfig` - 顶层配置
  - `arch/Kconfig` - 架构选择
  - `arch/arm/Kconfig` - ARM 特定配置
  - `arch/arm64/Kconfig` - ARM64 特定配置
  - `drivers/Kconfig` - 驱动配置
  - `fs/Kconfig` - 文件系统配置
  - `board/Kconfig` - 板级配置

- **defconfig 文件**：默认配置
  - `arch/<ARCH>/configs/defconfig` - 架构默认配置
  - `arch/<ARCH>/configs/<board>_defconfig` - 板级默认配置

- **生成的文件**：
  - `out/.config` - 当前配置
  - `out/include/autoconf.h` - 自动生成的 C 头文件

## 配置选项类型

### bool
布尔选项，可以是 `y`（启用）或 `n`（禁用）。

```kconfig
config MMU
    bool "Enable MMU support"
    default n
```

### tristate
三态选项，可以是 `y`（内建）、`m`（模块）或 `n`（禁用）。

```kconfig
config USB
    tristate "USB support"
    default n
```

### int
整数选项，支持范围限制。

```kconfig
config MAX_PRIORITY
    int "Maximum task priority levels"
    default 32
    range 8 256
```

### hex
十六进制选项。

```kconfig
config KERNEL_ADDR
    hex "Kernel load address"
    default 0x08000000
```

### string
字符串选项。

```kconfig
config DEFAULT_CONSOLE
    string "Default console device"
    default "tty0"
```

## 依赖关系和约束

### depends on
选项只有在依赖条件满足时才可见。

```kconfig
config USB_HID
    bool "USB HID support"
    depends on USB
```

### select
选项被启用时自动启用其他选项。

```kconfig
config ARCH_ARM64
    bool "ARM64 (64-bit)"
    select ARCH_HAS_MMU
```

### 复杂依赖
支持逻辑运算符：

```kconfig
config ADVANCED_FEATURE
    bool "Advanced feature"
    depends on (ARM && MMU) || ARM64
```

## 选择组（choice）

用于互斥选项，确保只能选择一个。

```kconfig
choice
    prompt "Target Architecture"
    default ARCH_ARM

config ARCH_ARM
    bool "ARM (32-bit)"

config ARCH_ARM64
    bool "ARM64 (64-bit)"

endchoice
```

## 菜单组织

### 基本菜单
```kconfig
menu "Memory Management"

config MMU
    bool "Enable MMU support"

config MM_DEBUG
    bool "Memory management debugging"

endmenu
```

### 条件菜单
```kconfig
menu "USB Configuration"
    depends on USB

config USB_HID
    bool "HID support"

endmenu
```

## 条件编译

在 C 代码中使用生成的宏：

```c
#include <autoconf.h>

#ifdef CONFIG_MMU
    /* MMU 相关代码 */
#endif

#if CONFIG_MAX_PRIORITY > 32
    /* 高优先级支持 */
#endif

#ifdef CONFIG_USB
    #ifdef CONFIG_USB_HID
        /* USB HID 代码 */
    #endif
#endif
```

## 高级特性

### 1. 配置验证
系统会自动验证：
- 依赖关系一致性
- 选择组互斥性
- 数值范围有效性

### 2. 智能默认值
支持条件默认值：

```kconfig
config FEATURE_X
    bool "Feature X"
    default y if ARCH_ARM
    default n
```

### 3. 帮助文本
每个选项都可以包含详细说明：

```kconfig
config MMU
    bool "Enable MMU support"
    help
      启用内存管理单元（MMU）支持。
      提供虚拟内存、内存保护等功能。
      
      如果不确定，选择 N。
```

## 迁移指南

### 从旧配置系统迁移

旧的配置文件（`arch/arm/config/*_config`）仍然受支持，但建议迁移到新的 Kconfig 系统。

迁移步骤：
1. 使用旧配置文件生成配置：`make nk60-v2_config`
2. 使用 menuconfig 调整配置：`make menuconfig`
3. 保存为新的 defconfig：`make savedefconfig`

## 故障排除

### Q: menuconfig 显示 "没有可用的选项"

A: 检查依赖关系是否满足。某些选项可能因为依赖条件不满足而不可见。

### Q: 配置保存后不生效

A: 确保重新编译内核：`make clean && make`

### Q: 如何添加新的配置选项？

A: 在相应的 Kconfig 文件中添加配置定义：

```kconfig
config MY_FEATURE
    bool "My new feature"
    depends on SOME_DEPENDENCY
    help
      Description of the feature.
```

### Q: 如何创建新板的配置？

A: 
1. 复制现有的 defconfig：`cp arch/arm/configs/defconfig arch/arm/configs/myboard_defconfig`
2. 使用 menuconfig 调整：`make menuconfig`
3. 保存配置：`make savedefconfig`
4. 重命名：`mv arch/arm/configs/defconfig arch/arm/configs/myboard_defconfig`

## 配置文件格式

### Kconfig 文件格式
```kconfig
# 注释
config SYMBOL_NAME
    type "Prompt text"
    default value
    depends on DEPENDENCY
    select SELECTED_SYMBOL
    help
      Help text here.
```

### defconfig 文件格式
```
# 注释
CONFIG_SYMBOL=y
CONFIG_STRING_SYMBOL="value"
CONFIG_INT_SYMBOL=123
# CONFIG_DISABLED_SYMBOL is not set
```

## 参考资料

- Linux Kconfig 语言文档：https://www.kernel.org/doc/html/latest/kbuild/kconfig-language.html
- NOS 内核设计文档：`.kiro/specs/kernel-architecture-refactor/design.md`
- 配置系统源码：`scripts/kconfig_improved.py`, `scripts/menuconfig_improved.py`
