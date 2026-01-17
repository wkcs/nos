#
# ARM64 架构 Makefile
#

# 架构特定的编译选项
ARCH_CFLAGS := -march=armv8-a

# 根据 CPU 类型设置编译选项
ifeq ($(CONFIG_ARM64_CORTEX_A53),y)
    ARCH_CFLAGS += -mcpu=cortex-a53
endif

ifeq ($(CONFIG_ARM64_CORTEX_A72),y)
    ARCH_CFLAGS += -mcpu=cortex-a72
endif

ifeq ($(CONFIG_ARM64_GENERIC),y)
    ARCH_CFLAGS += -mcpu=generic
endif

# 页面大小配置
ifeq ($(CONFIG_ARM64_4K_PAGES),y)
    ARCH_CFLAGS += -DCONFIG_ARM64_4K_PAGES
endif

ifeq ($(CONFIG_ARM64_64K_PAGES),y)
    ARCH_CFLAGS += -DCONFIG_ARM64_64K_PAGES
endif

# 虚拟地址空间配置
ARCH_CFLAGS += -DCONFIG_ARM64_VA_BITS=$(CONFIG_ARM64_VA_BITS)
ARCH_CFLAGS += -DCONFIG_ARM64_PA_BITS=$(CONFIG_ARM64_PA_BITS)

# 导出架构特定的编译选项
export ARCH_CFLAGS

# 子目录
obj-y += boot/
obj-y += kernel/
obj-y += mm/
obj-y += lib/