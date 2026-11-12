# SPDX-License-Identifier: GPL-2.0-only
obj-m := nbdriver.o
nbdriver-y := nbdriver_main.o \
	      nbdriver_comm.o \
	      nbdriver_entry.o \
	      nbdriver_entry_reboot.o \
	      nbdriver_ksym.o \
	      nbdriver_log.o
ccflags-y += -I$(src)/../include
