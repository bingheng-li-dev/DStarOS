#!/bin/bash
# 启动 QEMU。固件默认用 QEMU 自带的 OpenSBI；换固件：SBI_BIOS=<固件路径> bash scripts/run.sh

# ROOTFS_ADDR 必须等于 memtype.h 的 ROOTFS_PHYS_BASE，否则镜像会静默写进 PMM 的页帧池。
# 镜像不存在时不装载，内核会自己格式化一张空盘。
ROOTFS_IMG=build/rootfs.img
ROOTFS_ADDR=0x87000000
LOADER=()
if [ -f "$ROOTFS_IMG" ]; then
    LOADER=(-device "loader,file=$ROOTFS_IMG,addr=$ROOTFS_ADDR")
else
    echo "run: $ROOTFS_IMG not found, booting without rootfs" >&2
fi

qemu-system-riscv64 \
    -M  virt \
    -m  128M   \
    -bios "${SBI_BIOS:-default}" \
    -smp ${SMP:-4}  \
    -kernel build/kernel.elf    \
    "${LOADER[@]}" \
    -nographic
