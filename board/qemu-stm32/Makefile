#
# QEMU STM32 板级 Makefile
#

CCFLAGS += -I$(dir)/inside-include
CCFLAGS += -I$(dir)/usb

# 板级初始化与配置
obj-y += init.o
obj-y += board_config.o
obj-y += board_mm.o

# 板级外设胶水（UART/时钟/GPIO 等）；SPL 已迁至 soc/stm32f1
obj-y += arch_dma.o
obj-y += arch_uart.o
obj-y += arch_clk.o
obj-y += arch_irq.o
obj-y += arch_gpio.o
obj-y += arch_console.o
obj-y += arch_boot.o
obj-$(CONFIG_USB) += arch_usb.o
obj-$(CONFIG_USB) += usb

# start.S 保留在 board，启动向量目前由 arch/arm/boot 提供，避免 Default_Handler 重复
# obj-y += start.o

# 链接脚本
LDSCRIPT += board/qemu-stm32/nos_kernel.ld.S
