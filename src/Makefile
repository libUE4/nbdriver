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

# Same legacy-CFI detection as lsdriver; keep callback CFI/KCFI enabled.
ifneq ($(filter -fsanitize=cfi,$(CC_FLAGS_CFI)),)
ccflags-y += -DNBDRIVER_LEGACY_CFI
endif
