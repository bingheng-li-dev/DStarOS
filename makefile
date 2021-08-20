
TOOLPATH?=/home/lbh/.platformio/packages/toolchain-kendryte210/bin
TOOLPREFIX?=$(TOOLPATH)/riscv64-unknown-elf-

# TOOLPREFIX?=riscv64-unknown-elf-

PLATFORM?=QEMU

CC := $(TOOLPREFIX)gcc
AS := $(TOOLPREFIX)as
LD := $(TOOLPREFIX)ld
OBJCOPY := $(TOOLPREFIX)objcopy
OBJDUMP := $(TOOLPREFIX)objdump

CFLAGS := -O -ggdb3
CFLAGS += -nostdlib -fno-pic -Wall -Werror
CFLAGS += -mcmodel=medany -march=rv64imafdc -mabi=lp64d
CFLAGS += -ffreestanding -fno-common -mno-relax
CFLAGS += -D $(PLATFORM)

LDSCRIPT:=./lds/linker_qemu.ld
LDFLAGS := -z max-page-size=4096

KERNEL_ELF:=kernel.elf
KERNEL_BIN:=kernel.bin

SRC_BASE := src
OUTDIR := build

INC_DIR:= kernel/include lib/bsp/include lib/core/include lib/drivers/include debug
SRC_DIR:= kernel lib/bsp lib/core lib/drivers debug

INC_DIR:=$(foreach n,$(INC_DIR),$(SRC_BASE)/$(n))
CFLAGS+=$(foreach n,$(INC_DIR),-I$(n))

SRC_DIR:=$(foreach n,$(SRC_DIR),$(SRC_BASE)/$(n))
C_OUTDIR:=$(addprefix $(OUTDIR)/,$(SRC_DIR))

C_SRC_C := $(foreach n,$(SRC_DIR),$(wildcard $(n)/*.c))
C_SRC_S := $(foreach n,$(SRC_DIR),$(wildcard $(n)/*.S))
C_OBJS_C := $(patsubst %.c,%.o,$(C_SRC_C))
C_OBJS_S := $(patsubst %.S,%.o,$(C_SRC_S))

C_OBJS := $(C_OBJS_C) $(C_OBJS_S)
C_OBJS := $(addprefix $(OUTDIR)/,$(C_OBJS))

KERNEL_ELF := $(OUTDIR)/$(KERNEL_ELF)
KERNEL_BIN := $(OUTDIR)/$(KERNEL_BIN)
LDFLAGS += -T $(LDSCRIPT) -o $(KERNEL_ELF)

.PHONY: all clean

all: $(KERNEL_ELF)

C_OBJS:$(C_OBJS_C) $(C_OBJS_S)

$(C_OUTDIR):
	mkdir -p $@

$(C_OBJS_C): %.o:%.c $(C_OUTDIR)
	$(CC) $(CFLAGS) -c $< -o $(OUTDIR)/$@
$(C_OBJS_S): %.o:%.S $(C_OUTDIR)
	$(CC) $(CFLAGS) -c $< -o $(OUTDIR)/$@

$(KERNEL_ELF): C_OBJS
	$(LD) $(LDFLAGS) $(C_OBJS)
	$(OBJCOPY) $(KERNEL_ELF) --strip-all -O binary $(KERNEL_BIN)

clean:
	rm -fv $(C_OBJS) $(KERNEL_ELF) $(KERNEL_BIN)
