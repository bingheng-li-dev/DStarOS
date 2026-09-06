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

.PHONY: all clean

all: hello.elf fork_wait.elf exectest.elf filetest.elf pipetest.elf ttytest.elf memtest.elf sigtest.elf timetest.elf argvtest.elf segtest.elf

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

clean:
	rm -f hello.elf fork_wait.elf exectest.elf filetest.elf pipetest.elf ttytest.elf memtest.elf sigtest.elf timetest.elf argvtest.elf segtest.elf
