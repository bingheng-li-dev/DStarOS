/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _ELF_H_
#define _ELF_H_

#include <stdint.h>

#include "vmm.h"

#define EI_NIDENT   16  /* ELF16字节头部 */
#define	EI_MAG0		0   /* e_ident[] indexes */
#define	EI_MAG1		1
#define	EI_MAG2		2
#define	EI_MAG3		3
#define	EI_CLASS	4
#define	EI_DATA		5
#define	EI_VERSION	6
#define	EI_OSABI	7
#define	EI_PAD		8

#define	ELFMAG0		0x7f		/* EI_MAG */
#define	ELFMAG1		'E'
#define	ELFMAG2		'L'
#define	ELFMAG3		'F'
#define	ELFMAG		"\177ELF"
#define	SELFMAG		4

/* e_ident[EI_CLASS] 用来标识对应的ELF文件类别 */
#define	ELFCLASSNONE	0		/* EI_CLASS */
#define	ELFCLASS32	1
#define	ELFCLASS64	2
#define	ELFCLASSNUM	3

/* e_ident[EI_DATA] 用来区分字节序 */
#define ELFDATANONE	0		/* e_ident[EI_DATA] */
#define ELFDATA2LSB	1
#define ELFDATA2MSB	2

/* e_ident[EI_VERSION] 目标文件格式的版本，目前就是EV_CURRENT，也就是1 */
#define EV_NONE		0		/* e_version, EI_VERSION */
#define EV_CURRENT	1
#define EV_NUM		2

/* e_machine 标识目标架构 */
#define EM_RISCV	243	/* RISC-V */

/* 段类型 */
#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4
#define PT_SHLIB   5
#define PT_PHDR    6
#define PT_TLS     7

/* 段权限标志 */
#define PF_R		0x4
#define PF_W		0x2
#define PF_X		0x1

typedef uint64_t Elf64_Addr;
typedef uint64_t Elf64_Off;
typedef uint16_t Elf64_Half;
typedef uint32_t Elf64_Word;
typedef int32_t  Elf64_Sword;
typedef uint64_t Elf64_Xword;
typedef int64_t  Elf64_Sxword;

/* 一个 ELF 文件头 */
struct elf64_hdr
{
    unsigned char e_ident[EI_NIDENT]; /* 魔数、类别、字节序、版本、OS/ABI */
    Elf64_Half e_type;                /* ET_REL / ET_EXEC / ET_DYN */
    Elf64_Half e_machine;             /* EM_RISCV */
    Elf64_Word e_version;
    Elf64_Addr e_entry;               /* 程序入口虚拟地址 */
    Elf64_Off  e_phoff;               /* 程序头表的文件偏移 */
    Elf64_Off  e_shoff;               /* 节头表的文件偏移 */
    Elf64_Word e_flags;               /* RISC-V 用于标识 ISA 子集与 ABI */
    Elf64_Half e_ehsize;
    Elf64_Half e_phentsize;           /* 程序头表每项字节数，固定 56 */
    Elf64_Half e_phnum;
    Elf64_Half e_shentsize;
    Elf64_Half e_shnum;
    Elf64_Half e_shstrndx;            /* .shstrtab 在节头表中的下标 */
};
typedef struct elf64_hdr Elf64_Ehdr;

/* 一个 ELF 段（segment） */
struct elf64_phdr {
    Elf64_Word  p_type;   /* PT_LOAD / PT_PHDR / ... */
    Elf64_Word  p_flags;  /* PF_R / PF_W / PF_X */
    Elf64_Off   p_offset; /* 段内容的文件偏移 */
    Elf64_Addr  p_vaddr;
    Elf64_Addr  p_paddr;  /* 内核忽略 */
    Elf64_Xword p_filesz; /* 段在文件中的字节数 */
    Elf64_Xword p_memsz;  /* 段在内存中的字节数，多出 p_filesz 的部分（.bss）清零 */
    Elf64_Xword p_align;
};
typedef struct elf64_phdr Elf64_Phdr;

/**
 * @brief elf_load 的出参：进入新程序所需的全部信息
 * @details phdr_va 是程序头表在新地址空间里的虚拟地址，不是文件偏移；
 *   musl 的 __init_tls 靠 AT_PHDR/AT_PHENT/AT_PHNUM 找 PT_TLS 段。
 *   求不出来时 phdr_va 与 phnum 一起置 0——要么全对要么全 0，
 *   填一半会让 musl 拿着假地址去解引用。
 */
typedef struct elf_info
{
    virAddr_t entry;    /* 程序入口，即 e_entry */
    virAddr_t phdr_va;  /* 程序头表的用户虚拟地址；0 表示求不出 */
    uint16_t  phent;    /* 单个程序头的字节数，即 e_phentsize */
    uint16_t  phnum;    /* 程序头个数；phdr_va 为 0 时本字段同为 0 */
} elf_info_t;

/* 把内存里的 ELF 镜像装进 mm；校验失败返回负的错误码 */
int elf_load(mm_t *mm, const unsigned char *image, uint64_t size, elf_info_t *info);

#endif /* _ELF_H_ */
