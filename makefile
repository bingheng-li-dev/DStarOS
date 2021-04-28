OBJS =                                       \
	Startup.o                              \
	tinyprintf.o                         \
	main.o

INDIR = ./os/src
OUTDIR = ./build

TOOLPREFIX=riscv64-unknown-elf-

CC = $(TOOLPREFIX)gcc
AS = $(TOOLPREFIX)as
LD = $(TOOLPREFIX)ld
OBJCOPY = $(TOOLPREFIX)objcopy
OBJDUMP = $(TOOLPREFIX)objdump

CFLAGS = -Wall -Werror -O -ggdb
CFLAGS += -mcmodel=medany -march=rv64imafdc -mabi=lp64d
CFLAGS += -ffreestanding -fno-common -nostdlib -mno-relax
CFLAGS += -I./os/include

LDFLAGS = -z max-page-size=4096

# all: kernelImage
# kernelImage: $(INDIR)/*.c $(INDIR)/*.S
# 	$(LD) $(LDFLAGS) -T ./lds/linker_qemu.ld -o kernel.elf $(OBJS)
# 	$(OBJCOPY) kernel.elf --strip-all -O binary $@

# $(OBJS): %.c %.S
# 	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f .d *.o

.PHONY: all kernelImage clean

