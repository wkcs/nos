##############################################
# Copyright (C) 2023-2023 胡启航<Nick Hu>
#
# Author: 胡启航<Nick Hu>
#
# Email: huqihan@live.com
##############################################

CCFLAGS += -I$(dir)/inside-include
CCFLAGS += -I$(dir)/drivers/inc
CCFLAGS += -I$(dir)/usb

obj-y = drivers
obj-$(CONFIG_USB) += usb

obj-y += arch_dma.o
obj-y += arch_uart.o
obj-y += arch_clk.o
obj-y += arch_irq.o
obj-y += arch_gpio.o
obj-y += board_config.o
obj-y += arch_console.o
obj-$(CONFIG_USB) += arch_usb.o
obj-y += system_stm32f10x.o
obj-y += init.o
obj-y += board_mm.o
obj-y += arch_boot.o

obj-y += start.o





