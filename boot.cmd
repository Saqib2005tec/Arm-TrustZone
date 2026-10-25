fatload virtio 0:1 0x40400000 Image
fatload virtio 0:1 0x44000000 initramfs.cpio.gz
setenv bootargs "console=ttyAMA0 earlycon rdinit=/init"
booti 0x40400000 0x44000000:$filesize $fdtcontroladdr
