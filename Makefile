KERNELDIR ?= /lib/modules/$(shell uname -r)/build

PWD := $(shell pwd)

TEST_DIR := test

ifneq ($(KERNELRELEASE),)

obj-m := mmapdr.o

mmapdr-objs := mmapdr_main.o \
               mmapdr_mmap.o \
               mmapdr_debug.o

ccflags-y := -DDEBUG -Werror

else

.PHONY: all modules clean load unload stats test help

all: modules

modules:
	@echo "  Building mmapdr.ko against kernel $(shell uname -r)"
	$(MAKE) -C $(KERNELDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KERNELDIR) M=$(PWD) clean
	@rm -f $(TEST_DIR)/mmapdr_test
	@echo "  Cleaned build artifacts"

load: modules
	@echo "  Loading mmapdr module..."
	sudo insmod mmapdr.ko
	@echo "  Module loaded. Device node:"
	@ls -la /dev/mmapdr 2>/dev/null || echo "  (udev may need a moment)"
	@echo ""
	@echo "  Kernel log:"
	dmesg | tail -5

unload:
	@echo "  Unloading mmapdr module..."
	sudo rmmod mmapdr
	@echo "  Module unloaded."
	dmesg | tail -3

reload: unload load

stats:
	@echo "=== mmapdr driver statistics ==="
	@cat /sys/kernel/debug/mmapdr/stats 2>/dev/null || \
		echo "Module not loaded or debugfs not mounted"
	@echo ""
	@echo "=== Page map ==="
	@cat /sys/kernel/debug/mmapdr/page_map 2>/dev/null || true

hexdump:
	@echo "=== DMA buffer contents (first 256 bytes) ==="
	@cat /sys/kernel/debug/mmapdr/hexdump 2>/dev/null || \
		echo "Module not loaded or debugfs not mounted"

dmesg:
	@dmesg | grep mmapdr

test: $(TEST_DIR)/mmapdr_test
	@echo "  Running mmapdr userspace test..."
	sudo $(TEST_DIR)/mmapdr_test

$(TEST_DIR)/mmapdr_test: $(TEST_DIR)/mmapdr_test.c mm.h
	@echo "  Compiling userspace test..."
	$(CC) -Wall -Wextra -g -I. -o $@ $<

help:
	@echo "mmapdr driver build system"
	@echo ""
	@echo "  make              Build the kernel module"
	@echo "  make clean        Remove all build artifacts"
	@echo "  make load         Insert module into running kernel (sudo)"
	@echo "  make unload       Remove module from running kernel (sudo)"
	@echo "  make reload       Unload then reload (sudo)"
	@echo "  make stats        Show debugfs statistics"
	@echo "  make hexdump      Show DMA buffer contents"
	@echo "  make test         Build and run userspace test (sudo)"
	@echo "  make dmesg        Show kernel log for this module"
	@echo ""
	@echo "  KERNELDIR=<path>  Override kernel source directory"

endif
