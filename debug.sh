qemu-system-aarch64	\
-nographic		\
-kernel arch/arm64/boot/Image	\
-append "console=ttyAMA0 nokaslr init=/sbin/init"	\
-cpu cortex-a72	\
-machine virt,virtualization=off	\
-initrd ramdisk.img	\
-m 1024 \
-s -S
