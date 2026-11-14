.DEFAULT_GOAL := all

PROJECT_DIR := $(abspath $(dir $(lastword $(MAKEFILE_LIST))))
KDIR ?=
ARCH ?= arm64
LLVM ?= 1
LLVM_IAS ?= 1
TUSI_NDK ?= $(HOME)/workspace/tusi-ndk/android-ndk-aide
SMOKE_CC ?= clang-18
TUSI_PREBUILT := $(TUSI_NDK)/toolchains/llvm/prebuilt/linux-arm
SMOKE_OUTPUT ?= $(PROJECT_DIR)/tests/nbdriver_smoke

LOADER_OUTPUT ?= $(PROJECT_DIR)/out/nbdriver-loader

.PHONY: all smoke loader clean check-source check-kdir help

check-source:
	@grep -q '^obj-m' "$(PROJECT_DIR)/src/Makefile" || { echo "尚无参与编译的模块：请在 src/Makefile 定义 obj-m 与对象列表" >&2; exit 1; }

check-kdir:
	@test -n "$(KDIR)" || { echo "请指定 KDIR=/path/to/kernel/build" >&2; exit 1; }
	@test -f "$(KDIR)/Makefile" || { echo "KDIR 中不存在内核 Makefile" >&2; exit 1; }

all: check-source check-kdir
	$(MAKE) -C "$(KDIR)" M="$(PROJECT_DIR)/src" ARCH=$(ARCH) LLVM=$(LLVM) LLVM_IAS=$(LLVM_IAS) modules
	mkdir -p "$(PROJECT_DIR)/out"
	cp "$(PROJECT_DIR)/src/nbdriver.ko" "$(PROJECT_DIR)/out/nbdriver.ko"

smoke:
	mkdir -p "$(dir $(SMOKE_OUTPUT))"
	$(SMOKE_CC) --target=aarch64-linux-android21 \
		--sysroot="$(TUSI_PREBUILT)/sysroot" \
		-resource-dir="$(TUSI_PREBUILT)/lib/clang/21" \
		-rtlib=compiler-rt -fuse-ld=lld -std=c11 -D_GNU_SOURCE \
		-O2 -Wall -Wextra -Werror -pthread -Wl,-z,max-page-size=16384 \
		-I"$(PROJECT_DIR)/include" -I"$(PROJECT_DIR)/sdk/include" \
		"$(PROJECT_DIR)/tests/nbdriver_smoke.c" "$(PROJECT_DIR)/sdk/src/client.c" \
		-o "$(SMOKE_OUTPUT)"

loader:
	mkdir -p "$(dir $(LOADER_OUTPUT))"
	$(SMOKE_CC) -std=c11 -Wall -Wextra -Werror \
		-I"$(PROJECT_DIR)/sdk/include" \
		"$(PROJECT_DIR)/tools/nbdriver_loader.c" \
		-o "$(LOADER_OUTPUT)"

clean: check-source check-kdir
	$(MAKE) -C "$(KDIR)" M="$(PROJECT_DIR)/src" ARCH=$(ARCH) LLVM=$(LLVM) LLVM_IAS=$(LLVM_IAS) clean
	$(RM) "$(PROJECT_DIR)/out/nbdriver.ko"

help:
	@echo "make [all|loader|clean] KDIR=/path/to/kernel/build [ARCH=arm64 LLVM=1 LLVM_IAS=1]"
	@echo "make smoke [TUSI_NDK=/path/to/android-ndk-aide SMOKE_CC=clang-18 SMOKE_OUTPUT=tests/nbdriver_smoke]"
