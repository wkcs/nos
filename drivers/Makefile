##############################################
# Copyright (C) 2023-2023 胡启航<Nick Hu>
#
# Author: 胡启航<Nick Hu>
#
# Email: huqihan@live.com
##############################################

obj-y += base
# char/block 目录在重组后暂缺；去掉以免把缺失子目录当目标文件编译
# obj-y += char
# obj-y += block
obj-y += key
obj-y += misc
obj-y += serial
obj-y += spi
obj-y += i2c
# USB off on slim qemu-stm32 golden; subdir objects already config-gated
obj-$(CONFIG_USB) += usb
# display/*.o already gated by LCD/OLED/RGB configs (none on golden)
obj-y += display
obj-$(CONFIG_STM32F4XX) += stm32f4xx
