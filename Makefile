# Copyright (C) 2026 NOS contributors
.DEFAULT_GOAL := all
PLATFORM ?= arm64_virt
LIBC ?= minimal
O ?= out/microkernel/$(PLATFORM)/$(LIBC)
-include out/microkernel/config.mk
include scripts/Makefile.microkernel
.PHONY: legacy
legacy:
	$(MAKE) -f scripts/Makefile.legacy $(LEGACY_TARGET)
