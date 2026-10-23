# Copyright (C) 2026 NOS contributors. Invoke from the repository root.
HOSTCC ?= cc
NLP_HOST_OUT ?= out/microkernel/nlp-host
NLP_SOURCES := $(wildcard servers/nlp/*/*.c) $(wildcard apps/nlp-host/*.c)
NLP_VENDOR := third_party/tinycrypt/lib/source/sha256.c third_party/tinycrypt/lib/source/utils.c
NLP_HEADERS := $(wildcard include/nos/nlp/*.h apps/nlp-host/*.h third_party/tinycrypt/lib/include/tinycrypt/*.h)
.PHONY: all check-registry
all: check-registry $(NLP_HOST_OUT)/nlp-server
check-registry:
	python3 scripts/gen_nlp_registry.py --check
$(NLP_HOST_OUT)/nlp-server: $(NLP_SOURCES) $(NLP_VENDOR) $(NLP_HEADERS) apps/nlp-host/Makefile
	@mkdir -p $(NLP_HOST_OUT)
	$(HOSTCC) -std=c11 -Wall -Wextra -Werror -O2 -g -Iinclude/nos -Ithird_party/tinycrypt/lib/include $(NLP_SOURCES) $(NLP_VENDOR) -lcrypto -lutil -o $@
