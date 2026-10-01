#!/bin/bash
# 启动 QEMU 并停在第一条指令，等 GDB 连 localhost:1234。换固件：SBI_BIOS=<固件路径> bash scripts/forgdb.sh

# ROOTFS_ADDR 必须等于 memtype.h 的 ROOTFS_PHYS_BASE，否则镜像会静默写进 PMM 的页帧池。
ROOTFS_IMG=build/rootfs.img
ROOTFS_ADDR=0x87000000
LOADER=()
if [ -f "$ROOTFS_IMG" ]; then
    LOADER=(-device "loader,file=$ROOTFS_IMG,addr=$ROOTFS_ADDR")
else
    echo "forgdb: $ROOTFS_IMG not found, booting without rootfs" >&2
fi

qemu-system-riscv64 \
    -M  virt \
    -m  128M   \
    -bios "${SBI_BIOS:-default}" \
    -smp ${SMP:-4}  \
    -kernel build/kernel.elf    \
    "${LOADER[@]}" \
    -nographic -s -S
