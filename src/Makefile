# SPDX-License-Identifier: GPL-2.0-only
obj-m := nbdriver.o
nbdriver-y := nbdriver_core.o \
              nbdriver_features.o \
              nbdriver_runtime.o \
              nbdriver_platform.o

# Shared protocol plus the single private-header include root.
ccflags-y += -I$(src)/../include -I$(src)

# Same legacy-CFI detection as lsdriver; keep callback CFI/KCFI enabled.
ifneq ($(filter -fsanitize=cfi,$(CC_FLAGS_CFI)),)
ccflags-y += -DNBDRIVER_LEGACY_CFI
endif
