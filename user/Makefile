# 用户态程序独立构建流水线（不参与内核主 makefile）。
# 产物 hello.elf 由 elf.c 的 elf_load() 解析加载；嵌入内核的字节数组见
# src/kernel/user_elf.c，重新生成方法见 doc/Phase1-U态基础设施开发计划.md。

TOOLPATH?=/root/riscv/toolchain-kendryte210/bin
TOOLPREFIX?=$(TOOLPATH)/riscv64-unknown-elf-

CC := $(TOOLPREFIX)gcc
OBJCOPY := $(TOOLPREFIX)objcopy

CFLAGS := -O -nostdlib -ffreestanding -fno-common -mno-relax
CFLAGS += -mcmodel=medlow -march=rv64imac -mabi=lp64
CFLAGS += -Wall -Werror

LDSCRIPT := user.ld
LDFLAGS := -T $(LDSCRIPT) -static

.PHONY: all clean

all: hello.elf fork_wait.elf exectest.elf filetest.elf pipetest.elf

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

clean:
	rm -f hello.elf fork_wait.elf exectest.elf filetest.elf pipetest.elf
