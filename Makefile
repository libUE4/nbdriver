# SPDX-License-Identifier: GPL-2.0-only
.DEFAULT_GOAL := all

PROJECT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
KDIR ?=
ARCH ?= arm64
LLVM ?= 1
LLVM_IAS ?= 1

.PHONY: all clean check-kdir help

check-kdir:
	@test -n "$(KDIR)" || { echo "请指定 KDIR=/path/to/kernel/build" >&2; exit 1; }
	@test -f "$(KDIR)/Makefile" || { echo "KDIR 中不存在内核 Makefile" >&2; exit 1; }

all: check-kdir
	$(MAKE) -C "$(KDIR)" M="$(PROJECT_DIR)/src" ARCH=$(ARCH) LLVM=$(LLVM) LLVM_IAS=$(LLVM_IAS) modules
	mkdir -p "$(PROJECT_DIR)/out"
	cp "$(PROJECT_DIR)/src/nbdriver.ko" "$(PROJECT_DIR)/out/nbdriver.ko"

clean: check-kdir
	$(MAKE) -C "$(KDIR)" M="$(PROJECT_DIR)/src" ARCH=$(ARCH) LLVM=$(LLVM) LLVM_IAS=$(LLVM_IAS) clean
	$(RM) "$(PROJECT_DIR)/out/nbdriver.ko"

help:
	@echo "make [all|clean] KDIR=/path/to/kernel/build [ARCH=arm64 LLVM=1 LLVM_IAS=1]"
