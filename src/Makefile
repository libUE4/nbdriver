# SPDX-License-Identifier: GPL-2.0-only
# 七个实现对象显式列出；src/nbdriver_*.h 是私有声明，不单独编译。
obj-m := nbdriver.o
nbdriver-y := nbdriver_core.o \
              nbdriver_features.o \
              nbdriver_runtime.o \
              nbdriver_platform.o \
              nbdriver_sf_groups.o \
              nbdriver_udp.o \
              nbdriver_touch.o

# 公共 ABI 来自 sdk/include/；私有声明从 src/ 引用，保持内核与用户态协议一致。
ccflags-y += -I$(src)/../sdk/include -I$(src)

# Same legacy-CFI detection as lsdriver; keep callback CFI/KCFI enabled.
ifneq ($(filter -fsanitize=cfi,$(CC_FLAGS_CFI)),)
ccflags-y += -DNBDRIVER_LEGACY_CFI
endif
