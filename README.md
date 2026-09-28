# DStarOS

RISC-V rv64 内核，运行在 QEMU `virt` 与 StarFive VisionFive 2（JH7110）实机上，用户态跑 BusyBox。

> 本文目前只记录日常用法，其余内容后续补充。

---

## 日常用法

### 1. 开发环境

构建与 QEMU 都在 Docker 容器 `dstar-v4` 里进行，源码以卷挂载进容器，宿主机上的修改即时可见。

首次创建容器（命令保存在 `scripts/docker_container`，挂载路径按本机实际位置修改）：

```
docker run -itd -v D:\riscv-os\DStarOS\:/root/riscv/DStarOS -v D:\riscv-os\riscv-os\toolchain-kendryte210:/root/riscv/toolchain-kendryte210 --name dstar-v4 riscv-os-devkit:v4
```

进入容器：

```bash
docker exec -it dstar-v4 /bin/bash
cd /root/riscv/DStarOS
```

以下命令除注明"U-Boot"或"板上 shell"的以外，都在容器内的仓库根目录执行。

> Windows Git Bash 里直接 `docker exec` 传 Unix 路径时，前面加 `MSYS_NO_PATHCONV=1`，否则路径会被改写。

### 2. QEMU：构建与运行

```bash
make                  # 构建内核：build/kernel.elf、build/kernel.bin
make rootfs           # 构建根文件系统镜像 build/rootfs.img（含用户程序与 BusyBox）
bash scripts/run.sh   # 启动 QEMU（默认 4 核；SMP=2 bash scripts/run.sh 改核数）
```

退出 QEMU：`Ctrl-A` 然后 `X`。

- 没有 `build/rootfs.img` 时 QEMU 照样能起，内核会现格式化一张空盘，但进不了 BusyBox。
- 调试开关都在 `src/debug/debug.h`，改完需要重新 `make`。

### 3. QEMU：回归

回归套件由 `src/debug/debug.h` 里的 `DEBUG_SUITE` 选择，取值是套件名的大写加 `SUITE_` 前缀：

```bash
# 例：跑 memtest
#   1. 把 debug.h 里的 DEBUG_SUITE 改成 SUITE_MEM
make
bash scripts/regress.sh mem        # 可加次数：bash scripts/regress.sh mem 5
#   2. 跑完改回 SUITE_NONE

bash scripts/regress_all.sh        # 或者一次跑完全部 19 套，自动切换并复位
bash scripts/regress_all.sh mem bb # 只跑指定几套
```

套件：`sched` `slab` `dcache` `vfs`（内核态），`file` `pipe` `tty` `mem` `exec` `sig` `time` `seg` `wait`
`trap` `musl` `msys` `mroot` `mfp` `bb`（用户态）。

- 通过时输出形如 `[mem] run 1: === memtest done: 54 pass  0 fail ===`；失败时打印 `FAILED` 及原因，完整串口日志存到容器的 `/tmp/regress-<套件>-<次>-fail.log`。
- **交付形态**：`DEBUG_SUITE` 为 `SUITE_NONE`。提交代码前确认已复位。

### 4. VF2：构建镜像

```bash
make vf2img           # 以 PLATFORM=VF2 全量重建，产出 build/vf2-kernel.img（会校验 Image 头）
make rootfs           # 根文件系统与 QEMU 共用同一个 build/rootfs.img
```

板子使用板载 flash 里出厂的 OpenSBI v1.2 + U-Boot，**不替换固件**；SD 卡只需要一个 FAT 分区，放 `vf2-kernel.img` 与 `rootfs.img`。

### 5. VF2：U-Boot 环境变量

以下变量已 `saveenv` 在板子上：

| 变量 | 内容 | 用途 |
|---|---|---|
| `bootcmd` | `run dstar_sd` | 上电默认从 SD 卡启动 |
| `dstar_sd` | `fatload mmc 1:1 0x40200000 vf2-kernel.img && fatload mmc 1:1 0x47000000 rootfs.img && booti 0x40200000 - ${fdtcontroladdr}` | 内核与 rootfs 都从 SD 卡读 |
| `dstar` | `tftpboot 0x40200000 vf2-kernel.img && tftpboot 0x47000000 rootfs.img && booti 0x40200000 - ${fdtcontroladdr}` | 内核与 rootfs 都走 TFTP |
| `dstar_t` | `tftpboot 0x40200000 vf2-${suite}.img && fatload mmc 1:1 0x47000000 rootfs.img && booti 0x40200000 - ${fdtcontroladdr}` | 板上回归：测试内核走 TFTP，rootfs 读 SD 卡 |

在 U-Boot 里改变量时注意三点，它们出错都**不会报错**：

- 串联命令用 `&&`，不要用 `;`——`;` 会吞掉前一步的失败，最后只看到一句不相干的 `Bad Linux RISCV Image magic!`；
- `setenv` 后面必须紧跟变量名，只给一个参数等于删除变量；
- 定义完变量，**在 `run`、`booti` 或断电之前先 `saveenv`**，否则重新上电就没了。

### 6. VF2：日常迭代（TFTP）

宿主机准备（只做一次）：

- 有线网卡设静态 IP，例如 `192.168.120.100/24`，**不设网关**（否则会抢走默认路由）；
- TFTP 服务（如 tftpd64）根目录指向仓库的 `build/`，监听接口选这张有线网卡；防火墙放行 UDP 69；
- 板子上已设 `ipaddr=192.168.120.230`、`serverip=192.168.120.100`、`tftpwindowsize=8`（按自己的网络修改）。

每轮迭代：

1. 容器里 `make vf2img`；
2. 板子上电，**倒计时（2 秒）内按任意键**进入 U-Boot；
3. `run dstar`。

### 7. VF2：更新 SD 卡里的内核或 rootfs

不用拆卡，在 U-Boot 里通过 TFTP 拉取后写进卡里，再读回核对：

```
tftpboot 0x40200000 vf2-kernel.img && crc32 0x40200000 ${filesize}
fatwrite mmc 1:1 0x40200000 vf2-kernel.img ${filesize}
mw.b 0x40200000 0 ${filesize}
fatload mmc 1:1 0x40200000 vf2-kernel.img && crc32 0x40200000 ${filesize}
```

两次 `crc32` 应一致。更新 rootfs 同理，把地址换成 `0x47000000`、文件名换成 `rootfs.img`。

### 8. VF2：板上回归

1. 容器里打包测试镜像：

   ```bash
   bash tools/build_vf2_suites.sh            # 全部 19 套 → build/vf2-<suite>.img
   bash tools/build_vf2_suites.sh wait mfp   # 只重打指定的几套
   ```

   脚本逐个核对开关、告警、特征串与 CRC，最后输出 `=== 全部镜像核对通过 ===` 才能上板；结束时会自动复位开关并重建交付镜像。

2. 板子插网线，倒计时内按键进入 U-Boot，每一套：

   ```
   setenv suite mem
   run dstar_t
   ```

3. 看串口上的 `done: N pass 0 fail` 收尾行，与 QEMU 的结果对照。测试跑完内核会停住（这版固件的 `sbi_shutdown` 不工作），**断电再上电**开始下一套。

`tty` 套件需要从 stdin 喂一段精确的输入序列，板上手敲对不上，跳过。

### 9. VF2：恢复出厂启动方式

倒计时内按键进入 U-Boot：

```
setenv bootcmd 'run load_vf2_env;run importbootenv;run load_distro_uenv;run boot2;run distro_bootcmd'
saveenv
```

单引号必须保留——原值带分号，不加引号会被拆成几条命令当场执行。只想临时走一次 TFTP 的话，直接 `run dstar` 即可，不必改 `bootcmd`。

---

## License

DStarOS 以 **GPL-3.0-or-later** 发布，全文见 [LICENSE](LICENSE)。

仓库中包含的第三方源码——Canaan K210 BSP、tinyprintf、Linux 的链表与红黑树、FatFs——
各自保留原作者的版权与许可证，逐一列在 [THIRD-PARTY.md](THIRD-PARTY.md)；涉及的许可证
全文存放在 `LICENSES/` 目录。

RustSBI 固件与 BusyBox 的源码和二进制均未包含在本仓库中。
