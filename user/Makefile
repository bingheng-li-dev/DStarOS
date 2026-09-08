# 用户态程序独立构建流水线（不参与内核主 makefile）。
# 产物 hello.elf 由 elf.c 的 elf_load() 解析加载；嵌入内核的字节数组见
# src/kernel/user_elf.c（该文件头部注释里有重新生成方法）。

TOOLPATH?=/root/riscv/toolchain-kendryte210/bin
TOOLPREFIX?=$(TOOLPATH)/riscv64-unknown-elf-

CC := $(TOOLPREFIX)gcc
OBJCOPY := $(TOOLPREFIX)objcopy

CFLAGS := -O -nostdlib -ffreestanding -fno-common -mno-relax
CFLAGS += -mcmodel=medlow -march=rv64imac -mabi=lp64
CFLAGS += -Wall -Werror

LDSCRIPT := user.ld
LDFLAGS := -T $(LDSCRIPT) -static

# segtest 专用：紧凑布局 + 16 字节页粒度，逼出两个 PT_LOAD 共享同一物理页的 ELF。
# 链接器默认会把段各自推到页边界，不加 max-page-size 就造不出这个形状。
LDSCRIPT_DENSE := user_dense.ld
LDFLAGS_DENSE := -T $(LDSCRIPT_DENSE) -static -Wl,-z,max-page-size=16

# ---- musl 程序（m*.c）----
# 用第二套工具链：target 是 Linux、配 musl，syscall 直接 ecall。
# **不用 user.ld**：musl 的 crt1.o 与默认链接脚本自成一套，硬套我们的脚本会把
# .init_array/.preinit_array 之类丢掉，__libc_start_main 就跑不起来。
# 默认链接地址已经是 0x10000，与裸机那批程序一致，内核侧不用区别对待。
# ABI 是 rv64gc/lp64d（详见 Phase9 §3.1，现成 musl 工具链只有这一种）——
# 内核不保存 FP 上下文、进 U 态时把 sstatus.FS 关死，所以这些程序**一旦真的
# 发出浮点指令就会被 SIGILL 杀掉**。编完用 check-mhello 目标核对。
MUSL_TOOLPATH ?= /root/riscv/toolchain-musl/bin
MUSL_PREFIX ?= $(MUSL_TOOLPATH)/riscv64-linux-
MUSL_CC := $(MUSL_PREFIX)gcc
MUSL_OBJDUMP := $(MUSL_PREFIX)objdump
MUSL_STRIP := $(MUSL_PREFIX)strip
MUSL_CFLAGS := -O2 -static -Wall -Werror

.PHONY: all clean

all: hello.elf fork_wait.elf exectest.elf filetest.elf pipetest.elf ttytest.elf memtest.elf sigtest.elf timetest.elf argvtest.elf segtest.elf waittest.elf mhello.elf msyscheck.elf mrootfs.elf

hello.elf: hello.c $(LDSCRIPT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ hello.c
	$(OBJCOPY) --strip-all $@ $@

fork_wait.elf: fork_wait.c $(LDSCRIPT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ fork_wait.c
	$(OBJCOPY) --strip-all $@ $@

exectest.elf: exectest.c $(LDSCRIPT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ exectest.c
	$(OBJCOPY) --strip-all $@ $@

filetest.elf: filetest.c $(LDSCRIPT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ filetest.c
	$(OBJCOPY) --strip-all $@ $@

pipetest.elf: pipetest.c $(LDSCRIPT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ pipetest.c
	$(OBJCOPY) --strip-all $@ $@

ttytest.elf: ttytest.c $(LDSCRIPT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ ttytest.c
	$(OBJCOPY) --strip-all $@ $@

memtest.elf: memtest.c $(LDSCRIPT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ memtest.c
	$(OBJCOPY) --strip-all $@ $@

sigtest.elf: sigtest.c $(LDSCRIPT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ sigtest.c
	$(OBJCOPY) --strip-all $@ $@

timetest.elf: timetest.c $(LDSCRIPT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ timetest.c
	$(OBJCOPY) --strip-all $@ $@

argvtest.elf: argvtest.c $(LDSCRIPT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ argvtest.c
	$(OBJCOPY) --strip-all $@ $@

segtest.elf: segtest.c $(LDSCRIPT_DENSE)
	$(CC) $(CFLAGS) $(LDFLAGS_DENSE) -o $@ segtest.c
	$(OBJCOPY) --strip-all $@ $@

waittest.elf: waittest.c $(LDSCRIPT)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ waittest.c
	$(OBJCOPY) --strip-all $@ $@

mhello.elf: mhello.c
	$(MUSL_CC) $(MUSL_CFLAGS) -o $@ mhello.c
	$(MUSL_STRIP) $@

msyscheck.elf: msyscheck.c
	$(MUSL_CC) $(MUSL_CFLAGS) -o $@ msyscheck.c
	$(MUSL_STRIP) $@

mrootfs.elf: mrootfs.c
	$(MUSL_CC) $(MUSL_CFLAGS) -o $@ mrootfs.c
	$(MUSL_STRIP) $@

# 核对 musl 程序里有没有真的用到浮点指令：内核不保存 FP 上下文，用到就会被 SIGILL 杀。
# 输出为空即安全。
.PHONY: check-mhello
check-mhello: mhello.elf
	@$(MUSL_OBJDUMP) -d $< | grep -oE '\b(f(add|sub|mul|div|sqrt|sgnj|min|max|cvt|mv|eq|lt|le|class|madd|msub|nmadd|nmsub)\.[sd]|f(l|s)[wd])\b' | sort | uniq -c; echo "  (上面为空 = 没有浮点指令)"

clean:
	rm -f hello.elf fork_wait.elf exectest.elf filetest.elf pipetest.elf ttytest.elf memtest.elf sigtest.elf timetest.elf argvtest.elf segtest.elf waittest.elf mhello.elf msyscheck.elf mrootfs.elf
