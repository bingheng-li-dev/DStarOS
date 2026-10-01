# 内核构建：make [PLATFORM=QEMU|VF2]，产出 build/kernel.elf 与 build/kernel.bin
# 换工具链：make TOOLPREFIX=<路径>/riscv64-unknown-elf-
TOOLPREFIX ?= riscv64-unknown-elf-

PLATFORM ?= QEMU
VALID_PLATFORMS := QEMU VF2
ifeq ($(filter $(PLATFORM),$(VALID_PLATFORMS)),)
$(error PLATFORM=$(PLATFORM) is invalid, choose from: $(VALID_PLATFORMS))
endif

CC      := $(TOOLPREFIX)gcc
LD      := $(TOOLPREFIX)ld
OBJCOPY := $(TOOLPREFIX)objcopy

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

SRC_BASE := src
OUTDIR   := build
KERNEL_ELF := $(OUTDIR)/kernel.elf
KERNEL_BIN := $(OUTDIR)/kernel.bin

INC_DIR := $(addprefix $(SRC_BASE)/,kernel/include lib/bsp/include lib/core/include lib/drivers/include lib/fatfs/include debug)
SRC_DIR := $(addprefix $(SRC_BASE)/,kernel lib/bsp lib/core lib/drivers lib/fatfs debug)
CFLAGS  += $(addprefix -I,$(INC_DIR))
C_OUTDIR := $(addprefix $(OUTDIR)/,$(SRC_DIR))

C_SRC_C := $(foreach n,$(SRC_DIR),$(wildcard $(n)/*.c))
C_SRC_S := $(foreach n,$(SRC_DIR),$(wildcard $(n)/*.S))
C_OBJS  := $(patsubst %.c,$(OUTDIR)/%.o,$(C_SRC_C)) $(patsubst %.S,$(OUTDIR)/%.o,$(C_SRC_S))
C_DEPS  := $(C_OBJS:.o=.d)

LDFLAGS := -z max-page-size=4096 -T $(LDSCRIPT) -o $(KERNEL_ELF)

# 平台宏变化不会改变任何 .o 的时间戳，换平台时必须删掉旧目标文件，否则会静默复用上一个平台的产物。
# 戳在删除的同时更新：它记录的是 build/ 里的 .o 属于哪个平台，而不是上次构建是否成功。
PLATFORM_STAMP := $(OUTDIR)/.platform
PREV_PLATFORM := $(shell cat $(PLATFORM_STAMP) 2>/dev/null)
ifneq ($(PREV_PLATFORM),)
ifneq ($(PREV_PLATFORM),$(PLATFORM))
$(info makefile: platform changed from $(PREV_PLATFORM) to $(PLATFORM), rebuilding everything)
$(shell rm -f $(C_OBJS) $(C_DEPS) $(KERNEL_ELF) $(KERNEL_BIN))
$(shell mkdir -p $(OUTDIR) && echo $(PLATFORM) > $(PLATFORM_STAMP))
endif
endif

.PHONY: all debug debugbuild clean rootfs vf2img

all: $(KERNEL_ELF)

# 已有的 .o 不会带上调试信息，一般用 debugbuild
debug: CFLAGS += -ggdb3
debug: $(KERNEL_ELF)

debugbuild:
	$(MAKE) clean
	$(MAKE) debug

$(OUTDIR)/%.o: %.c | $(C_OUTDIR)
	$(CC) $(CFLAGS) -c $< -o $@

# FatFS 是第三方代码，不改源码：它有几个没进头文件的全局函数
$(OUTDIR)/src/lib/fatfs/ff.o: CFLAGS += -Wno-missing-prototypes

$(OUTDIR)/%.o: %.S | $(C_OUTDIR)
	$(CC) $(CFLAGS) -Wa,-gdwarf-2 -c $< -o $@

$(C_OUTDIR):
	mkdir -p $@

$(KERNEL_ELF): $(C_OBJS)
	$(LD) $(LDFLAGS) $(C_OBJS)
	$(OBJCOPY) $(KERNEL_ELF) --strip-all -O binary $(KERNEL_BIN)
	@echo $(PLATFORM) > $(PLATFORM_STAMP)

# 不挂进 all：rootfs 依赖 user/ 下用 musl 工具链编好的程序
rootfs:
	bash tools/build_rootfs.sh

vf2img:
	bash tools/build_vf2_image.sh

clean:
	rm -fv $(C_OBJS) $(C_DEPS) $(KERNEL_ELF) $(KERNEL_BIN) $(PLATFORM_STAMP)

-include $(C_DEPS)
