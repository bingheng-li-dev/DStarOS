# 启动 QEMU 并停在第一条指令，等 GDB 连 localhost:1234。固件默认用 QEMU 自带的 OpenSBI（-bios default），与 VF2 板载固件同源；
# 换固件：SBI_BIOS=<固件路径> bash scripts/forgdb.sh

# rootfs 镜像由 QEMU 的 -device loader 原样写进内存的 ROOTFS_PHYS_BASE。
# 这个地址必须与 src/kernel/include/memtype.h 的 ROOTFS_PHYS_BASE 一致，
# 对不上的话镜像会落在 PMM 的页帧池里，那是静默的内存损坏。
#
# 镜像不存在时不加这个参数：内核的 fatfs_mount 会退回 f_mkfs 现格式化一张空盘，
# 所有不依赖镜像内容的回归照常跑。硬加的话没跑过 `make rootfs` 的人连内核都起不来。
ROOTFS_IMG=build/rootfs.img
ROOTFS_ADDR=0x87000000
LOADER=()
if [ -f "$ROOTFS_IMG" ]; then
    LOADER=(-device "loader,file=$ROOTFS_IMG,addr=$ROOTFS_ADDR")
else
    echo "未找到 $ROOTFS_IMG，本次不装载 rootfs（内核会自己 f_mkfs 一张空盘）" >&2
fi

qemu-system-riscv64 \
    -M  virt \
    -m  128M   \
    -bios "${SBI_BIOS:-default}" \
    -smp ${SMP:-4}  \
    -kernel build/kernel.elf    \
    "${LOADER[@]}" \
    -nographic -s -S