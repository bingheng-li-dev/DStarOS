
# 裸机工具链默认取 PATH 里的 riscv64-unknown-elf-（容器内由 gcc-riscv64-unknown-elf 提供），
# 用别的工具链时 make TOOLPREFIX=<路径>/riscv64-unknown-elf-
TOOLPREFIX?=riscv64-unknown-elf-

# 目标平台，只支持 QEMU 与 VF2。
PLATFORM?=QEMU
VALID_PLATFORMS := QEMU VF2
ifeq ($(filter $(PLATFORM),$(VALID_PLATFORMS)),)
$(error PLATFORM=$(PLATFORM) 无效，可选：$(VALID_PLATFORMS))
endif

CC := $(TOOLPREFIX)gcc
AS := $(TOOLPREFIX)as
LD := $(TOOLPREFIX)ld
OBJCOPY := $(TOOLPREFIX)objcopy
OBJDUMP := $(TOOLPREFIX)objdump

CFLAGS := -O
CFLAGS += -nostdlib -fno-pic -Wall -Werror -Wmissing-prototypes -Wshadow
CFLAGS += -mcmodel=medany -march=rv64imafdc -mabi=lp64d
CFLAGS += -ffreestanding -fno-common -mno-relax
CFLAGS += -MMD -MP
CFLAGS += -D $(PLATFORM)

ifeq ($(PLATFORM),VF2)
LDSCRIPT := ./lds/linker_vf2.ld
else
LDSCRIPT := ./lds/linker_qemu.ld
endif
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

# 换平台必须全量重编。
# `-D $(PLATFORM)` 是编译期宏，但它变了不会让任何 .o 的时间戳变化，于是
# `make PLATFORM=VF2` 会打印 "Nothing to be done" 并静默复用上一个平台的目标文件——
# 你以为编了 VF2，拿到的是 QEMU 的产物。这属于"静默给错东西"，比编译失败危险得多
# （头文件依赖 -MMD 也救不了：变的是命令行宏，不是任何一个文件）。
# 解法是把平台名戳进 build/.platform，发现和本次不一致就先把 .o 全删掉。
PLATFORM_STAMP := $(OUTDIR)/.platform
PREV_PLATFORM := $(shell cat $(PLATFORM_STAMP) 2>/dev/null)
ifneq ($(PREV_PLATFORM),)
ifneq ($(PREV_PLATFORM),$(PLATFORM))
$(info makefile: 平台由 $(PREV_PLATFORM) 变为 $(PLATFORM)，强制全量重编)
$(shell rm -f $(C_OBJS) $(C_DEPS) $(KERNEL_ELF) $(KERNEL_BIN))
# 戳必须在删完 .o 的当下就更新，不能等构建成功再写：这一趟只要链接失败，
# build/ 里就留下了"新平台的 .o + 旧平台的戳"，下次切回旧平台时判定为无需重编，
# 直接拿错平台的 .o 去链接——正是本机制要消灭的那类静默错误换了个触发路径。
# 戳记的是"build/ 里的 .o 属于哪个平台"，不是"上次成功构建的平台"。
$(shell mkdir -p $(OUTDIR) && echo $(PLATFORM) > $(PLATFORM_STAMP))
endif
endif

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

# FatFS 是第三方代码，不改源码：它有几个没进头文件的全局函数
$(OUTDIR)/src/lib/fatfs/ff.o: CFLAGS += -Wno-missing-prototypes

$(OUTDIR)/%.o: %.S | $(C_OUTDIR)
	$(CC) $(CFLAGS) -Wa,-gdwarf-2 -c $< -o $@

$(C_OUTDIR):
	mkdir -p $@

$(KERNEL_ELF): $(C_OBJS) | $(C_OUTDIR)
	$(LD) $(LDFLAGS) $(C_OBJS)
	$(OBJCOPY) $(KERNEL_ELF) --strip-all -O binary $(KERNEL_BIN)
	@echo $(PLATFORM) > $(PLATFORM_STAMP)

# 造根文件系统镜像（build/rootfs.img）。不挂进 all：它依赖 user/ 下已经编好的
# .elf，而 user/ 是独立于内核的构建流水线（见 user/Makefile），把两者绑在一起会让
# 只想编内核的人被迫先备齐 musl 工具链。需要时显式 `make rootfs`。
rootfs:
	bash tools/build_rootfs.sh

# 打包可被 U-Boot booti 启动的 VF2 内核镜像（含 64 字节 Image 头校验）。
# 脚本内部自己以 PLATFORM=VF2 全量重建，所以这里不加依赖。
.PHONY: vf2img
vf2img:
	bash tools/build_vf2_image.sh

clean:
	rm -fv $(C_OBJS) $(C_DEPS) $(KERNEL_ELF) $(KERNEL_BIN) $(PLATFORM_STAMP)

# 头文件依赖：没有它，改 pmm.h 里 pframe_t 的布局只会重编直接改动的 .c，
# 其余 .o 仍按旧 sizeof 编译，症状是完全无关的位置莫名其妙地挂掉
-include $(C_DEPS)