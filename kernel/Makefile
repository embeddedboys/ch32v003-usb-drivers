
# local kernel build dir
KERN_DIR:=/lib/modules/$(shell uname -r)/build

# users kernel dir
# KERN_DIR:=/home/user/linux

# CachyOS/Arch kernels are built with clang, so the modules have to be too.
# Pass LLVM=1 through if it was given, otherwise detect it.
ifeq ($(LLVM),1)
KBUILD_ARGS:=LLVM=1
else ifneq ($(shell grep -s '^CONFIG_CC_IS_CLANG=y' $(KERN_DIR)/.config),)
KBUILD_ARGS:=LLVM=1
else
KBUILD_ARGS:=
endif

# The MFD core (USB transport + cells) and the child drivers hang off it, the
# same split as drivers/mfd/dln2.c with drivers/gpio/gpio-dln2.c.  Child
# modules use symbols exported by the core, so they are loaded after it.
MODULE_NAME:=usb-mfd
GPIO_MODULE:=v003-gpio

all:
	make -C $(KERN_DIR) M=`pwd` $(KBUILD_ARGS) modules

clean:
	make -C $(KERN_DIR) M=`pwd` $(KBUILD_ARGS) clean

# convenience for testing on a bench machine
test: all
	sudo rmmod $(GPIO_MODULE) || true
	sudo rmmod $(MODULE_NAME) || true
	sudo insmod $(MODULE_NAME).ko || true
	sudo insmod $(GPIO_MODULE).ko || true
	dmesg | tail -20

# the core is a single source module (usb-mfd.c); naming the composite object
# after the module would make kbuild drop a circular dependency
obj-m += $(MODULE_NAME).o

obj-m += $(GPIO_MODULE).o
$(GPIO_MODULE)-y += gpio.o
