# DStarOS

[![CI](https://github.com/bingheng-li-dev/DStarOS/actions/workflows/ci.yml/badge.svg)](https://github.com/bingheng-li-dev/DStarOS/actions/workflows/ci.yml)

[中文](README.md) | English

A RISC-V rv64 kernel that runs on QEMU `virt` and on the StarFive VisionFive 2 (JH7110) board, with BusyBox in
userspace.

<img src="./preview.png" width="600" />

> Code comments and the detailed documentation are written in Chinese. This page is a short overview; see the
> [Chinese README](README.md) for the full guide (board setup, U-Boot, regression tests, project layout).

## Features

- **Memory management**: best-fit physical page allocator that merges adjacent free blocks; slab allocator behind
  `kmalloc`; Sv39 page tables, demand paging, copy-on-write `fork`
- **Processes and scheduling**: SMP (4 harts on QEMU and on the VF2's U74 cores); CFS, RT and idle scheduling
  classes; `fork`/`execve`/`wait4`, ELF loading
- **File systems**: Linux-style VFS (super_block / inode / dentry / file) with an LRU dentry cache; FatFS on a RAM
  disk (`/`) and on the VF2 SD card (`/sd`); devfs (`/dev`); block layer with an LRU buffer cache
- **IPC**: pipes, POSIX signals
- **Terminal**: TTY line discipline and termios
- **Userspace**: statically linked musl programs, BusyBox ash as the shell, `/sbin/init` as PID 1
- **Platforms**: QEMU `virt`; VisionFive 2 (UART, PLIC, SD card driver, device tree parsing)

## Quick start

The only host requirement is Docker with the Compose plugin (Docker Desktop on Windows). Toolchains, QEMU and GDB
are all inside the development image. From the repository root:

```bash
docker compose up -d          # builds the dev image on first run
docker compose exec dev bash  # enter the container
make && make -C user && bash tools/build_busybox.sh && make rootfs
bash scripts/run.sh           # boots into the BusyBox shell; Ctrl-A X quits QEMU
```

| Command | Output |
|---|---|
| `make` | kernel: `build/kernel.elf`, `build/kernel.bin` |
| `make -C user` | user programs: `user/*.elf` |
| `bash tools/build_busybox.sh` | `build/busybox` (downloads BusyBox 1.38.0 on first run) |
| `make rootfs` | root file system image `build/rootfs.img` |
| `make vf2img` | VisionFive 2 kernel image `build/vf2-kernel.img`, booted with U-Boot `booti` |
| `bash scripts/forgdb.sh` | QEMU waiting for GDB on port 1234 |
| `bash scripts/regress_all.sh` | runs all 19 regression suites |

On the VisionFive 2 the factory OpenSBI + U-Boot are kept; the kernel and `rootfs.img` are loaded from the SD card
or over TFTP.

## Known limitations

- No networking, no `/proc`; only statically linked programs (no dynamic linker).
- FAT file system: no symlinks or permission bits, ASCII file names only.
- No shutdown: quit QEMU with `Ctrl-A X`; power-cycle or press RST on the board.
- The BusyBox build is trimmed to 13 applets and has no job control or line editing; the ash builtins `read` and
  `wait` do not work yet (`ppoll` and `rt_sigsuspend` are not implemented).

## License

DStarOS is released under **GPL-3.0-or-later**, see [LICENSE](LICENSE). Third-party code (Canaan K210 BSP headers,
tinyprintf, Linux list and rbtree, FatFs) keeps its own copyright and license, listed in
[THIRD-PARTY.md](THIRD-PARTY.md).
