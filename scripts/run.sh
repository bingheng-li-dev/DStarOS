# For RustSBI v0.1.1
# qemu-system-riscv64 \
#     -M  virt \
#     -m  8M   \
#     -bios ./bootloader/RustSBI/v0.0.1/sbi-qemu \
#     -smp 2  \
#     -kernel build/kernel.elf    \
#     -nographic

# For OpenSBI
# qemu-system-riscv64 \
#     -M  virt \
#     -bios ./bootloader/fw_payload_qemu.bin \
#     -device loader,file=build/kernel.elf,addr=0x80200000 \
#     -nographic

#For RustSBI v0.4.0
qemu-system-riscv64 \
    -M  virt \
    -m  128M   \
    -bios ./bootloader/RustSBI/v0.4.0/rustsbi-prototyper-dynamic.elf \
    -smp 2  \
    -kernel build/kernel.elf    \
    -nographic