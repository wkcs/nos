#
# NK60 板级 Makefile (STM32F401 / Cortex-M4)
# SPL / CMSIS 已迁至 soc/stm32f4；此处仅板级绑定与胶水。
#

CCFLAGS += -I$(dir)/inside-include
CCFLAGS += -I$(dir)/usb

# 板级初始化与配置（生命周期集中在 init.c）
obj-y += init.o
obj-y += board_config.o
obj-y += board_mm.o

# 板级外设胶水（UART/时钟/GPIO 等）
obj-y += arch_dma.o
obj-y += arch_uart.o
obj-y += arch_clk.o
obj-y += arch_irq.o
obj-y += arch_gpio.o
obj-y += arch_console.o
obj-y += arch_boot.o
obj-$(CONFIG_USB) += arch_usb.o
obj-$(CONFIG_USB) += usb

# 启动向量由 arch/arm/boot/start.S 提供，避免与板级 start.S 重复
# obj-y += start.o

# 链接脚本
LDSCRIPT += board/nk60/nos_kernel.ld.S
