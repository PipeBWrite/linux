obj-$(CONFIG_INTEL_DSA_SL) += dsa.o

dsa-y := init.o dumps.o ctrl.o memcpy.o ioctl.o mm.o irq.o

dsa-$(CONFIG_INTEL_DSA_SL_ENABLE_USER_THREAD) += thread.o
dsa-$(CONFIG_INTEL_DSA_SL_EMU) += dsa_emu.o folio_pool.o sysfs_emu.o
