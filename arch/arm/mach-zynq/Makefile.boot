   zreladdr-y	+= 0x00008000
params_phys-y	:= 0x00000100
initrd_phys-y	:= 0x00800000

dtb-$(CONFIG_ARCH_ZYNQ) += \
	ni-vb80x2.dtb \
	ni-vb80x2-eth.dtb \
	ni-vb80x4.dtb \
