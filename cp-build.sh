make CROSS_COMPILE=aarch64-linux-gnu- ARCH=arm64 Image -j16
cp -v ./arch/arm64/boot/Image ~/gpu/GPU-SFTP/firecracker-bins/
