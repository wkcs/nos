# Copyright (C) 2026 NOS contributors
.DEFAULT_GOAL := all
PLATFORM ?= arm64_virt
ESP_IDF_PROFILE ?= runtime
LIBC ?= $(if $(filter esp32s3,$(PLATFORM)),idf,minimal)
O ?= $(if $(filter esp32s3,$(PLATFORM)),out/esp32s3/$(ESP_IDF_PROFILE),out/microkernel/$(PLATFORM)/$(LIBC))
-include out/microkernel/config.mk
ifeq ($(PLATFORM),esp32s3)
include scripts/Makefile.esp32s3
else
include scripts/Makefile.microkernel
endif
