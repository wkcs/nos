#
# QEMU STM32 板级 Makefile
#

# 板级初始化
obj-y += board.o
obj-y += init.o

# 板级配置
obj-y += board_config.o
obj-y += board_mm.o

# 链接脚本
LDSCRIPT += board/qemu-stm32/nos_kernel.ld.S