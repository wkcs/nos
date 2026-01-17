##############################################
# Copyright (C) 2023-2023 胡启航<Nick Hu>
#
# Author: 胡启航<Nick Hu>
#
# Email: huqihan@live.com
##############################################

VERSION = 1
PATCHLEVEL = 0
SUBLEVEL = 0

#打开显示选项
ifneq ($(V),1)
	Q := @
endif

ifneq ($(MV), 1)
	N := --no-print-directory
endif

ifndef $(ARCH)
    ARCH := arm
endif

# 设置工具链前缀
ifndef $(PREFIX)
    ifeq ($(ARCH),arm)
        PREFIX := arm-none-eabi-
    endif
    ifeq ($(ARCH),arm64)
        PREFIX := aarch64-none-elf-
    endif
endif

dir-y := init
dir-y += arch
dir-y += drivers
dir-y += kernel
dir-y += libs
dir-y += modules
dir-y += board
dir-y += app
dir-y += fs

dir-run := $(dir-y:%=%-run)
ifndef $(out-dir)
    out-dir := out
endif
obj-dir := $(dir-y:%=$(out-dir)/%)
obj-all := $(dir-y:%=$(out-dir)/%/built-in.o)

#链接脚本
LDSCRIPT :=

-include $(out-dir)/.config
include scripts/Makefile.config
include arch/$(ARCH)/Makefile.config
ifneq ($(CONFIG_BOARD),)
board := $(shell echo $(CONFIG_BOARD) | sed 's/\"//g')
-include board/$(board)/Makefile.config
endif

OOCD		:= openocd
OOCD_INTERFACE	:= flossjtag

LDSCRIPT_S := $(filter %.S,$(LDSCRIPT))
LDSCRIPT_S := $(LDSCRIPT_S:%.S=$(out-dir)/%)
LDSCRIPT_LD := $(filter-out %.S,$(LDSCRIPT))
LDSCRIPT_LD := $(LDSCRIPT_LD:%=$(out-dir)/%)
LDFLAGS += $(LDSCRIPT:%.S=-T$(out-dir)/%)

BOARD_SVD := $(out-dir)/board.svd
OPENOCD_CFG := $(out-dir)/openocd.cfg
OOCDFLAGS := -f $(OPENOCD_CFG)

# 设备树相关
DTC := dtc
BOARD_DTS := board/$(board)/board.dts
BOARD_DTB := $(out-dir)/board.dtb

.PHONY: all flash debug clean %_config defconfig menuconfig savedefconfig size stflash jflash check-compiler dtbs FORCE

all:$(TARGET_LIST) $(TARGET_BIN) $(TARGET_HEX) $(TARGET_ELF) $(TARGET_IMG) dtbs size

size: $(TARGET_ELF)
	@echo "SIZE     $(<:$(out-dir)/%=%)"
	$(Q)$(SIZE) --format=berkeley $<
	@echo build done

$(TARGET_HEX): $(TARGET_ELF)
	@echo "OBJCOPY  $(@:$(out-dir)/%=%)"
	$(Q)$(OBJCOPY) $<  $@ -Oihex

$(TARGET_BIN): $(TARGET_ELF)
	@echo "OBJCOPY  $(@:$(out-dir)/%=%)"
	$(Q)$(OBJCOPY) $<  $@ -Obinary

$(TARGET_IMG): $(TARGET_BIN)
	@echo "CREATE   $(@:$(out-dir)/%=%)"
	$(Q)-$(RM) $@
	$(Q)$(PYTHON) scripts/create_img.py $< $(VERSION) $(PATCHLEVEL) $(SUBLEVEL) 1 $@

# 设备树编译规则
dtbs: $(BOARD_DTB)

$(BOARD_DTB): $(BOARD_DTS) | $(out-dir)
	@echo "DTC      $(@:$(out-dir)/%=%)"
	$(Q)if [ -f $(BOARD_DTS) ]; then \
		$(DTC) -I dts -O dtb -o $@ $(BOARD_DTS); \
	else \
		echo "Warning: Device tree source $(BOARD_DTS) not found"; \
		touch $@; \
	fi

$(TARGET_LIST): $(TARGET_ELF)
	@echo "OBJDUMP  $(@:$(out-dir)/%=%)"
	$(Q)$(OBJDUMP) -S $< > $@

$(TARGET_ELF): $(obj-all) $(LDSCRIPT_S) $(LDSCRIPT_LD) $(BOARD_SVD) $(OPENOCD_CFG)
	@echo "GEN      empty kallsyms"
	$(Q)echo '#include <kernel/kallsyms.h>' > $(out-dir)/kallsyms_table.c
	$(Q)echo 'const struct kallsym_entry kallsyms_table[] = { {0, (const char *)0} };' >> $(out-dir)/kallsyms_table.c
	$(Q)echo 'const unsigned int kallsyms_num = 0;' >> $(out-dir)/kallsyms_table.c
	$(Q)$(CC) $(CCFLAGS) -o $(out-dir)/kallsyms_table.o $(out-dir)/kallsyms_table.c
	@echo "LD       $(@:$(out-dir)/%=%) (pass 1)"
	$(Q)$(CC) $(LDFLAGS) -o $@ $(obj-all) $(out-dir)/kallsyms_table.o $(LDFLAGS-LIB)
	@echo "GEN      real kallsyms"
	$(Q)$(OBJDUMP) -t $@ | $(PYTHON) scripts/mkkallsyms.py /dev/stdin $(out-dir)/kallsyms_table.c
	@echo "CC       kallsyms_table.o"
	$(Q)$(CC) $(CCFLAGS) -o $(out-dir)/kallsyms_table.o $(out-dir)/kallsyms_table.c
	@echo "LD       $(@:$(out-dir)/%=%) (relink)"
	$(Q)$(CC) $(LDFLAGS) -o $@ $(obj-all) $(out-dir)/kallsyms_table.o $(LDFLAGS-LIB)

$(LDSCRIPT_LD): $(out-dir)/%:%
	@echo "CP       $(notdir $@)"
	$(Q)cp $< $@

$(OPENOCD_CFG):
	@echo "CP       $@"
	$(Q)if [ -f "$(OOCDCFG)" ]; then \
		cp $(OOCDCFG) $@; \
	else \
		touch $@; \
	fi

$(BOARD_SVD):
	@echo "CP       $@"
	$(Q)if [ -f "$(SVD_CFG)" ]; then \
		cp $(SVD_CFG) $@; \
	else \
		touch $@; \
	fi


$(LDSCRIPT_S): $(out-dir)/%:%.S
	@echo "E        $(notdir $@)"
	$(Q)$(CC) -E $(INC_DIR) $(DEFINES) -P -o $@ $<

$(obj-all): $(obj-dir) $(out-dir)/.config $(out-dir)/include/autocfg.h FORCE
	$(Q)$(MAKE) $(N) -f scripts/Makefile.build dir=$(@:$(out-dir)/%/$(notdir $@)=%) \
		out-dir=$(out-dir) Q=$(Q) N=$(N) ARCH=$(ARCH) \
		TARGET=$(notdir $@) built-in.o

$(obj-dir):$(out-dir)
	$(Q)if [ ! -d $@ ]; then \
		mkdir $@; \
	fi

$(out-dir):
	$(Q)if [ ! -d $@ ]; then \
		mkdir $@; \
	fi

# 使用OpenOCD下载hex程序
flash: $(OPENOCD_CFG)
	@echo "OPEN_OCD FLASH $(TARGET_HEX:$(out-dir)/%=%)"
	$(Q)$(OOCD) $(OOCDFLAGS) -c "program $(TARGET_HEX) verify reset exit"

# 使用st-link下载bin程序
stflash:
	@echo "ST-FLASH     $(TARGET_BIN:$(out-dir)/%=%)"
	$(Q)st-flash --connect-under-reset write $(TARGET_BIN) 0x08000000

# 使用J-link下载hex程序
jflash:
	@echo "J-FLASH     $(TARGET_HEX:$(out-dir)/%=%)"
	$(Q)scripts/jflash.sh $(TARGET_HEX)

# 使用GDB 通过sdtin/out管道与OpenOCD连接 并在main函数处打断点后运行
debug: $(OPENOCD_CFG) $(BOARD_SVD)
	@echo "GDB DEBUG $(TARGET_ELF)"
	$(Q)$(GDB) -iex 'target extended | $(OOCD) $(OOCDFLAGS) -c "gdb_port pipe"' \
	-iex 'monitor reset halt' -ex 'load' -ex 'break nos_start' -ex 'c' $(TARGET_ELF)

debug-server: $(OPENOCD_CFG) $(BOARD_SVD)
	@echo "GDB DEBUG $(TARGET_ELF)"
	$(Q)$(OOCD) $(OOCDFLAGS) -c "gdb_port 1234"

qemu-run:
	$(Q)if [ ! "$(QEMU_CMD)" ]; then \
		echo "$(board) does not support running on qemu"; \
		exit 1; \
	fi
	$(Q)if [ ! "$(QEMU_DIR)" ]; then \
		$(QEMU_CMD) -kernel $(TARGET_ELF); \
	else \
		$(QEMU_DIR)/$(QEMU_CMD) -kernel $(TARGET_ELF); \
	fi

qemu-run-gdb: $(OPENOCD_CFG) $(BOARD_SVD)
	$(Q)if [ ! "$(QEMU_CMD)" ]; then \
		echo "$(board) does not support running on qemu"; \
		exit 1; \
	fi
	$(Q)if [ ! "$(QEMU_DIR)" ]; then \
		$(QEMU_CMD) -kernel $(TARGET_ELF) -S -s; \
	else \
		$(QEMU_DIR)/$(QEMU_CMD) -kernel $(TARGET_ELF) -S -s; \
	fi

%_config: $(obj-dir)
	@echo "Generating configuration from $@"
	$(Q)if [ -f arch/$(ARCH)/configs/$@ ]; then \
		$(PYTHON) scripts/kconfig_improved.py . arch/$(ARCH)/configs/$@ $(out-dir); \
	elif [ -f arch/$(ARCH)/config/$@ ]; then \
		$(PYTHON) scripts/config.py $(VERSION) $(PATCHLEVEL) $(SUBLEVEL) $(ARCH) arch/$(ARCH)/config/$@ $(out-dir)/.config; \
		if [ ! -d $(out-dir)/include ]; then mkdir $(out-dir)/include; fi; \
		$(PYTHON) scripts/autocfg.py $(out-dir)/.config $(out-dir)/include/autocfg.h; \
	else \
		echo "Error: Configuration file not found"; \
		exit 1; \
	fi

defconfig: $(obj-dir)
	@echo "Generating default configuration"
	$(Q)if [ -f arch/$(ARCH)/configs/defconfig ]; then \
		$(PYTHON) scripts/kconfig_improved.py . arch/$(ARCH)/configs/defconfig $(out-dir); \
	else \
		echo "Error: Default configuration not found"; \
		exit 1; \
	fi

menuconfig: $(obj-dir)
	@echo "Starting interactive configuration"
	$(Q)if [ ! -f $(out-dir)/.config ]; then \
		echo "No existing configuration found, using defaults"; \
		$(MAKE) defconfig; \
	fi
	$(Q)$(PYTHON) scripts/menuconfig_improved.py . $(out-dir)/.config

savedefconfig:
	@echo "Saving minimal configuration"
	$(Q)if [ ! -f $(out-dir)/.config ]; then \
		echo "Error: No configuration found. Run 'make defconfig' first."; \
		exit 1; \
	fi
	$(Q)$(PYTHON) scripts/savedefconfig.py . $(out-dir)/.config arch/$(ARCH)/configs/defconfig

clean:
	$(Q)-$(RM) $(out-dir)
	@echo "clean done"

# 编译器检查目标
check-compiler: $(out-dir)/.config
	@echo "检查编译器配置..."
	$(Q)if [ "$(CONFIG_CC_IS_CLANG)" = "y" ]; then \
		$(PYTHON) scripts/check_compiler.py clang $(ARCH) $(PREFIX); \
	else \
		$(PYTHON) scripts/check_compiler.py gcc $(ARCH) $(PREFIX); \
	fi
