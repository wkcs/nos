#
# ARM 架构 Makefile
#

# 架构特定的编译选项
ARCH_CFLAGS := -mthumb
ARCH_ASFLAGS := -mthumb

# 根据 CPU 类型设置编译选项
ifeq ($(CONFIG_ARM_CORTEX_M3),y)
    ARCH_CFLAGS += -mcpu=cortex-m3
    ARCH_ASFLAGS += -mcpu=cortex-m3
endif

ifeq ($(CONFIG_ARM_CORTEX_M4),y)
    ARCH_CFLAGS += -mcpu=cortex-m4
    ARCH_ASFLAGS += -mcpu=cortex-m4
    ifeq ($(CONFIG_ARM_CORTEX_M4_FPU),y)
        ARCH_CFLAGS += -mfloat-abi=hard -mfpu=fpv4-sp-d16
    endif
endif

ifeq ($(CONFIG_ARM_CORTEX_A7),y)
    ARCH_CFLAGS += -mcpu=cortex-a7
    ARCH_ASFLAGS += -mcpu=cortex-a7
endif

# Thumb-2 指令集支持
ifeq ($(CONFIG_THUMB2_KERNEL),y)
    ARCH_CFLAGS += -mthumb
    ARCH_ASFLAGS += -mthumb
endif

# 导出架构特定的编译选项
export ARCH_CFLAGS ARCH_ASFLAGS

# 子目录
obj-y += boot/
obj-y += kernel/
obj-y += mm/

# 架构初始化
obj-y += init.o

# CPU 特定代码
obj-y += cpu/