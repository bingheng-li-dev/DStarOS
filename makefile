
TOOLPATH?="/root/riscv/toolchain-kendryte210/bin"
TOOLPREFIX?=$(TOOLPATH)/riscv64-unknown-elf-

PLATFORM?=QEMU

CC := $(TOOLPREFIX)gcc
AS := $(TOOLPREFIX)as
LD := $(TOOLPREFIX)ld
OBJCOPY := $(TOOLPREFIX)objcopy
OBJDUMP := $(TOOLPREFIX)objdump

CFLAGS := -O
CFLAGS += -nostdlib -fno-pic -Wall -Werror
CFLAGS += -mcmodel=medany -march=rv64imafdc -mabi=lp64d
CFLAGS += -ffreestanding -fno-common -mno-relax
CFLAGS += -MMD -MP
CFLAGS += -D $(PLATFORM)

LDSCRIPT:=./lds/linker_qemu.ld
LDFLAGS := -z max-page-size=4096

KERNEL_ELF:=kernel.elf
KERNEL_BIN:=kernel.bin

SRC_BASE := src
OUTDIR := build

INC_DIR:= kernel/include lib/bsp/include lib/core/include lib/drivers/include lib/fatfs/include debug
SRC_DIR:= kernel lib/bsp lib/core lib/drivers lib/fatfs debug

INC_DIR:=$(foreach n,$(INC_DIR),$(SRC_BASE)/$(n))
CFLAGS+=$(foreach n,$(INC_DIR),-I$(n))

SRC_DIR:=$(foreach n,$(SRC_DIR),$(SRC_BASE)/$(n))
C_OUTDIR:=$(addprefix $(OUTDIR)/,$(SRC_DIR))

C_SRC_C := $(foreach n,$(SRC_DIR),$(wildcard $(n)/*.c))
C_SRC_S := $(foreach n,$(SRC_DIR),$(wildcard $(n)/*.S))

C_OBJS_C := $(patsubst %.c,$(OUTDIR)/%.o,$(C_SRC_C))
C_OBJS_S := $(patsubst %.S,$(OUTDIR)/%.o,$(C_SRC_S))
C_OBJS   := $(C_OBJS_C) $(C_OBJS_S)
C_DEPS   := $(C_OBJS:.o=.d)

KERNEL_ELF := $(OUTDIR)/$(KERNEL_ELF)
KERNEL_BIN := $(OUTDIR)/$(KERNEL_BIN)
LDFLAGS += -T $(LDSCRIPT) -o $(KERNEL_ELF)

.PHONY: all debug debugbuild clean rootfs

all: $(KERNEL_ELF)

# debug前需要clean掉之前的.o文件，否则可能会因为之前的.o文件没有调试信息而导致debug失败
debug: CFLAGS += -ggdb3
debug: $(KERNEL_ELF)

# 安全的调试构建：先clean再debug，避免混用无调试信息的.o文件
debugbuild:
	$(MAKE) clean
	$(MAKE) debug

# 用 order-only 依赖（|）创建输出目录：目录时间戳不触发重编
$(OUTDIR)/%.o: %.c | $(C_OUTDIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(OUTDIR)/%.o: %.S | $(C_OUTDIR)
	$(CC) $(CFLAGS) -Wa,-gdwarf-2 -c $< -o $@

$(C_OUTDIR):
	mkdir -p $@

$(KERNEL_ELF): $(C_OBJS)
	$(LD) $(LDFLAGS) $(C_OBJS)
	$(OBJCOPY) $(KERNEL_ELF) --strip-all -O binary $(KERNEL_BIN)

# 造根文件系统镜像（build/rootfs.img）。**不挂进 all**：它依赖 user/ 下已经编好的
# .elf，而 user/ 是独立于内核的构建流水线（见 user/Makefile），把两者绑在一起会让
# 只想编内核的人被迫先备齐 musl 工具链。需要时显式 `make rootfs`。
rootfs:
	bash tools/build_rootfs.sh

clean:
	rm -fv $(C_OBJS) $(C_DEPS) $(KERNEL_ELF) $(KERNEL_BIN)

# 头文件依赖：没有它，改 pmm.h 里 pframe_t 的布局只会重编直接改动的 .c，
# 其余 .o 仍按旧 sizeof 编译，症状是完全无关的位置莫名其妙地挂掉
-include $(C_DEPS)