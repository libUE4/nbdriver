# SPDX-License-Identifier: GPL-2.0-only
obj-m := nbdriver.o
nbdriver-y := nbdriver_main.o \
	      nbdriver_comm.o \
	      nbdriver_entry.o \
	      nbdriver_entry_reboot.o \
	      nbdriver_ksym.o \
	      nbdriver_cfi.o \
	      nbdriver_log.o
ccflags-y += -I$(src)/../include

# 编译目标 KMI 的实际 Module.symvers 决定是否可静态调用 misc API。
# 禁止因为函数在源码里出现，就猜它在厂商 ABI 中已导出。
nbdriver_misc_exports := $(shell if test -f "$(objtree)/Module.symvers"; then \
	awk '$$2 == "misc_register" || $$2 == "misc_deregister" { print $$2 }' \
		"$(objtree)/Module.symvers"; fi)
ifeq ($(sort $(nbdriver_misc_exports)),misc_deregister misc_register)
nbdriver-y += nbdriver_entry_misc.o
ccflags-y += -DNBDRIVER_HAS_MISC
endif

# Same legacy-CFI detection as lsdriver; keep callback CFI/KCFI enabled.
ifneq ($(filter -fsanitize=cfi,$(CC_FLAGS_CFI)),)
ccflags-y += -DNBDRIVER_LEGACY_CFI
endif

# 改写内核全局 CFI slowpath 并非兼容的必要条件；默认禁用。
# 仅允许经单独厂商内核审计后显式选择，不作为发行构建选项。
ifeq ($(NBDRIVER_ENABLE_GLOBAL_CFI_PATCH),1)
ccflags-y += -DNBDRIVER_ENABLE_GLOBAL_CFI_PATCH
endif
