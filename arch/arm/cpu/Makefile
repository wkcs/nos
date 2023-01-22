##############################################
# Copyright (C) 2023-2023 胡启航<Nick Hu>
#
# Author: 胡启航<Nick Hu>
#
# Email: huqihan@live.com
##############################################

cpu_type := $(shell echo $(CONFIG_CPU_TYPE) | sed 's/\"//g')
cpu_type_dir := $(shell if [ -d $(dir)/$(cpu_type) ]; then echo $(cpu_type); fi)

obj-y += $(cpu_type_dir)

obj-y += cpu.o
