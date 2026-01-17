##############################################
# Copyright (C) 2023-2024 Nick Hu
#
# 内核测试模块 Makefile
##############################################

obj-y += compiler_test.o

# 只有在启用测试时才编译
ifeq ($(CONFIG_KERNEL_TESTS),y)
    obj-y += test_main.o
    obj-y += test_directory_structure.o
endif