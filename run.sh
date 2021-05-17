qemu-system-riscv64 \
    -M  virt \
    -bios ./bootloader/fw_payload_qemu.bin \
    -device loader,file=build/kernel.elf,addr=0x80200000 \
    -nographic -s
