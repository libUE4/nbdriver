.DEFAULT_GOAL := all

PROJECT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
KDIR ?=
ARCH ?= arm64
LLVM ?= 1
LLVM_IAS ?= 1

.PHONY: all clean check-source check-kdir help

check-source:
	@grep -q '^obj-m' "$(PROJECT_DIR)/src/Makefile" || { echo "尚无参与编译的模块：请在 src/Makefile 定义 obj-m 与对象列表" >&2; exit 1; }

check-kdir:
	@test -n "$(KDIR)" || { echo "请指定 KDIR=/path/to/kernel/build" >&2; exit 1; }
	@test -f "$(KDIR)/Makefile" || { echo "KDIR 中不存在内核 Makefile" >&2; exit 1; }

all: check-source check-kdir
	$(MAKE) -C "$(KDIR)" M="$(PROJECT_DIR)/src" ARCH=$(ARCH) LLVM=$(LLVM) LLVM_IAS=$(LLVM_IAS) modules
	mkdir -p "$(PROJECT_DIR)/out"
	cp "$(PROJECT_DIR)/src/nbdriver.ko" "$(PROJECT_DIR)/out/nbdriver.ko"

clean: check-source check-kdir
	$(MAKE) -C "$(KDIR)" M="$(PROJECT_DIR)/src" ARCH=$(ARCH) LLVM=$(LLVM) LLVM_IAS=$(LLVM_IAS) clean
	$(RM) "$(PROJECT_DIR)/out/nbdriver.ko"

help:
	@echo "make [all|clean] KDIR=/path/to/kernel/build [ARCH=arm64 LLVM=1 LLVM_IAS=1]"
