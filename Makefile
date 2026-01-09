KERNEL_RELEASE  ?= $(shell uname -r)
KERNEL_DIR      ?= /lib/modules/$(KERNEL_RELEASE)/build
DKMS_TARBALL    ?= dkms.tar.gz
TAR             ?= tar
obj-m           += lotspeed.o

ccflags-y := -std=gnu99

# 抑制一些编译器警告，保持 gcc/clang 兼容
ccflags-y += -Wno-declaration-after-statement
ccflags-y += -Wno-unused-function

# 检测内核版本 >= 6.11.0 时定义 LOTSPEED_NEW_CONG_CONTROL_API
# 6.11+ 的 cong_control 签名变为 (sk, ack, flag, rs)
ifeq ($(shell printf '%s\n%s\n' '6.11.0' '$(KERNEL_RELEASE)' | sort -V | head -n1),6.11.0)
ccflags-y += -DLOTSPEED_NEW_CONG_CONTROL_API
endif

.PHONY: all clean load unload
.PHONY: .always-make

all:
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) modules

clean: clean-dkms.conf clean-dkms-tarball
	$(MAKE) -C $(KERNEL_DIR) M=$(PWD) clean

load:
	sudo insmod lotspeed.ko

unload:
	sudo rmmod lotspeed

.PHONY: dkms-tarball clean-dkms-tarball clean-dkms.conf

.always.make:

dkms.conf: ./scripts/mkdkmsconf.sh .always-make
	./scripts/mkdkmsconf.sh > dkms.conf

clean-dkms.conf:
	$(RM) dkms.conf

$(DKMS_TARBALL): dkms.conf Makefile lotspeed.c
	$(TAR) zcf $(DKMS_TARBALL) \
		--transform 's,^,./dkms_source_tree/,' \
		dkms.conf \
		Makefile \
		lotspeed.c

dkms-tarball: $(DKMS_TARBALL)

clean-dkms-tarball:
	$(RM) $(DKMS_TARBALL)
