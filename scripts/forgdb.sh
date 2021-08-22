qemu-system-riscv64 \
    -M  virt \
    -m  8M   \
    -bios ./bootloader/RustSBI/sbi-qemu \
    -smp 2  \
    -kernel build/kernel.elf    \
    -nographic -s -S