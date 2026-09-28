/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "elf.h"
#include "console.h"
#include "stringops.h"
#include "errorcode.h"
#include "uaccess.h"

/* 可处理的 PT_LOAD 段数上限。真实可执行文件通常只有 2~4 个（RX / RO / RW），
 * 给 8 是为了下面两个定长数组能安心放在栈上——内核栈只有一页。 */
#define ELF_MAX_LOAD_SEG 8

/* 一个 PT_LOAD 段落到地址空间里的页对齐范围。
 * 合并阶段复用同一个类型来承载"若干段的并集"，那时只用 start/end/prot 三个字段。 */
typedef struct
{
    virAddr_t   start; /* round_down_page(p_vaddr) */
    virAddr_t   end;   /* round_up_page(p_vaddr + p_memsz) */
    pgprot_t    prot;
    Elf64_Phdr *ph;    /* 回填内容时要用；合并项不使用 */
} elf_seg_t;

/* 向下取整到页边界 */
static inline virAddr_t round_down_page(virAddr_t va)
{
    return va & ~(PGSIZE - 1);
}

/* 向上取整到页边界 */
static inline virAddr_t round_up_page(virAddr_t va)
{
    return (va + PGSIZE - 1) & ~(PGSIZE - 1);
}

/**
 * @brief 把内存里的 ELF 镜像装进 mm：按 PT_LOAD 建 VMA、映射、拷贝内容，再建堆 VMA
 */
int elf_load(mm_t *mm, const unsigned char *image, uint64_t size, elf_info_t *info)
{
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)image;

    /* ELF 来自磁盘、由用户 execve 指定，不可信：校验失败返回错误码交给调用者。
     * size 边界必须校验，越界的 e_phoff/p_offset/p_filesz 会读到 image 之外的内核内存；
     * p_vaddr 的范围也必须校验，第三遍是直接按 p_vaddr 写内存的。 */
    if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0)
    {
        printf("%s: bad ELF magic\n", __FUNCTION__);
        return ENO6_INVAL_PARAM;
    }
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS64 || ehdr->e_machine != EM_RISCV)
    {
        printf("%s: unsupported class/machine\n", __FUNCTION__);
        return ENO6_INVAL_PARAM;
    }
    if (ehdr->e_phnum == 0)
    {
        printf("%s: e_phnum is 0\n", __FUNCTION__);
        return ENO6_INVAL_PARAM;
    }
    /* e_phentsize 不是 sizeof(Elf64_Phdr) 时，下面按 sizeof(Elf64_Phdr) 步进的数组
     * 访问就会用错误的 stride 解析程序头表 */
    if (ehdr->e_phentsize != sizeof(Elf64_Phdr))
    {
        printf("%s: unexpected e_phentsize\n", __FUNCTION__);
        return ENO6_INVAL_PARAM;
    }
    /* 程序头表整体必须落在 [0, size) 内；先比较再相减，避免 e_phoff 接近
     * UINT64_MAX 时 e_phoff + 表长 发生整数回绕 */
    if (ehdr->e_phoff > size ||
        (uint64_t)ehdr->e_phnum * ehdr->e_phentsize > size - ehdr->e_phoff)
    {
        printf("%s: program header table out of bounds\n", __FUNCTION__);
        return ENO6_INVAL_PARAM;
    }

    Elf64_Phdr *phdr_base = (Elf64_Phdr *)(image + ehdr->e_phoff);
    /* AT_PHDR 的两条求法，优先 PT_PHDR（GNU ld 通常会生成），否则退回
     * "覆盖了 e_phoff 的那个 PT_LOAD 段" */
    virAddr_t pt_phdr_va = 0;
    virAddr_t phdr_in_load_va = 0;

    /* 装载分三遍走：先把地址空间的形状算清楚，再落实映射，最后才填内容。
     * 不能"一个 PT_LOAD 建一个 VMA、马上映射再拷贝"：段是文件的单位，页是地址空间的单位，
     * 相邻两段可能共用一页，后一段的映射会换掉那页、丢掉前一段的内容，两个 VMA 还会重叠。 */
    elf_seg_t segs[ELF_MAX_LOAD_SEG];
    int nload = 0;

    /* 第一遍：校验各 PT_LOAD，并记下它的页对齐落地范围（只算不落实） */
    for (int i = 0; i < ehdr->e_phnum; i++)
    {
        Elf64_Phdr *ph = &phdr_base[i];
        if (ph->p_type == PT_PHDR)
        {
            pt_phdr_va = ph->p_vaddr;
        }
        if (ph->p_type != PT_LOAD)
        {
            continue;
        }
        if (nload >= ELF_MAX_LOAD_SEG)
        {
            printf("%s: too many PT_LOAD segments\n", __FUNCTION__);
            return ENO6_INVAL_PARAM;
        }
        /* 程序头表通常落在第一个 PT_LOAD 段里（它从文件偏移 0 开始映射） */
        if (ehdr->e_phoff >= ph->p_offset && ehdr->e_phoff < ph->p_offset + ph->p_filesz)
        {
            phdr_in_load_va = ph->p_vaddr + (ehdr->e_phoff - ph->p_offset);
        }

        /* 段内容必须落在 [0, size) 内，否则下面的 memcpy 会读到 image 缓冲区之外。
         * 仅在 p_filesz > 0 时检查：纯 .bss 段不读文件，链接器可以把它的 p_offset 放在文件末尾之后。 */
        if (ph->p_filesz > 0 && (ph->p_offset > size || ph->p_filesz > size - ph->p_offset))
        {
            printf("%s: segment %d file range out of bounds\n", __FUNCTION__, i);
            return ENO6_INVAL_PARAM;
        }
        /* p_memsz < p_filesz 违反 ELF 规范；下面 p_memsz - p_filesz 是无符号减法，
         * 一旦发生会下溢成天文数字，喂给 memset 就是巨量越界写 */
        if (ph->p_memsz < ph->p_filesz)
        {
            printf("%s: segment %d p_memsz < p_filesz\n", __FUNCTION__, i);
            return ENO6_INVAL_PARAM;
        }
        /* p_vaddr + p_memsz 回绕的话 round_up_page 会算出一个比 start 还小的 end，
         * 建出来的 VMA 区间是空的，随后的 memcpy 却照写不误 */
        if (ph->p_memsz > (uint64_t)(-1) - ph->p_vaddr - PGSIZE)
        {
            printf("%s: segment %d address range overflows\n", __FUNCTION__, i);
            return ENO6_INVAL_PARAM;
        }
        /* 段必须整个落在用户区，并给紧跟其后的堆留出 USER_HEAP_MAX（堆不得碰到 mmap 区）。
         * 放过一个落在内核高半段的段，第三遍的 memcpy 就是在替用户改写内核内存。 */
        if (ph->p_vaddr + ph->p_memsz > USER_MMAP_BASE - USER_HEAP_MAX)
        {
            printf("%s: segment %d outside user space\n", __FUNCTION__, i);
            return ENO6_INVAL_PARAM;
        }
        /* ELF 规范要求 PT_LOAD 按 p_vaddr 升序排列且内存区间互不重叠。
         * 这两条是下面合并算法的前提（线性扫一遍即可），也是安全前提：
         * 区间一旦重叠，第三遍里后写的段会静默盖掉先写的段的内容。 */
        if (nload > 0)
        {
            Elf64_Phdr *prev = segs[nload - 1].ph;
            if (ph->p_vaddr < prev->p_vaddr + prev->p_memsz)
            {
                printf("%s: segment %d overlaps or is out of order\n", __FUNCTION__, i);
                return ENO6_INVAL_PARAM;
            }
        }

        /* 段权限统一额外加 VMP_W：第三遍要往刚映射好的页里拷内容，而 SUM 不豁免 PTE 的
         * R/W/X 位；没有"拷完再改回只读"的原语，所以用户代码段也可写（无 W^X，已知简化）。 */
        pgprot_t flag = VMP_W;
        if (ph->p_flags & PF_R)
        {
            flag |= VMP_R;
        }
        if (ph->p_flags & PF_X)
        {
            flag |= VMP_X;
        }

        segs[nload].start = round_down_page(ph->p_vaddr);
        segs[nload].end   = round_up_page(ph->p_vaddr + ph->p_memsz);
        segs[nload].prot  = flag;
        segs[nload].ph    = ph;
        nload++;
    }
    if (nload == 0)
    {
        printf("%s: no PT_LOAD segment\n", __FUNCTION__);
        return ENO6_INVAL_PARAM;
    }

    /* 第二遍：把各段的页范围并成互不重叠的区间，一个区间建一个 VMA、只映射一次。
     * 段已按 p_vaddr 升序，线性扫一遍即可完成合并。 */
    elf_seg_t merged[ELF_MAX_LOAD_SEG];
    int nmerged = 0;
    for (int i = 0; i < nload; i++)
    {
        if (nmerged > 0 && segs[i].start < merged[nmerged - 1].end)
        {
            if (segs[i].end > merged[nmerged - 1].end)
            {
                merged[nmerged - 1].end = segs[i].end;
            }
            /* 共享页同时属于前后两个段，权限取并集才能同时满足两边 */
            merged[nmerged - 1].prot |= segs[i].prot;
        }
        else
        {
            merged[nmerged] = segs[i];
            nmerged++;
        }
    }

    virAddr_t max_end = 0;
    for (int i = 0; i < nmerged; i++)
    {
        vma_t *vma = vmm_vma_create(merged[i].start, merged[i].end, merged[i].prot);
        if (!vma)
        {
            return ENO1_NOMORE_MEM;
        }
        vmm_vma_insert(mm, vma);
        if (vmm_map_vma(mm, vma) != ENO0_NO_ERROR)
        {
            return ENO1_NOMORE_MEM;
        }
        max_end = vma->vm_end; /* 合并结果按地址升序，最后一个自然是最高处 */
    }

    /* 第三遍：把文件内容填进已经映好的页。各段的字节区间互不重叠（第一遍已校验），
     * 所以填的先后与结果无关。
     * SUM 已开，且 satp 已切到该 mm，可直接写用户 VA。 */
    for (int i = 0; i < nload; i++)
    {
        Elf64_Phdr *ph = segs[i].ph;
        /* p_filesz == 0 时直接跳过——此时 image + p_offset 可能已经越过缓冲区末尾，
         * 虽然长度为 0 的 memcpy 不会解引用，但连这个越界指针都不去构造更干净。 */
        if (ph->p_filesz > 0)
        {
            memcpy((void *)ph->p_vaddr, image + ph->p_offset, ph->p_filesz);
        }
        /* 零填充 bss 区。vmm_map_vma 分配的帧本来就已清零，这一笔在当前实现下是
         * 冗余的；保留是因为它表达的是"bss 必须是零"这个契约，而不是"帧恰好是零"。 */
        memset((void *)ph->p_vaddr + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
    }

    info->entry   = ehdr->e_entry;
    info->phdr_va = (pt_phdr_va != 0) ? pt_phdr_va : phdr_in_load_va;
    /* 求不出程序头表地址时 phnum 必须一起清零，见 elf_info_t 的注释 */
    info->phent   = (info->phdr_va != 0) ? ehdr->e_phentsize : 0;
    info->phnum   = (info->phdr_va != 0) ? ehdr->e_phnum : 0;

    /* 堆 VMA 一次性建到最大尺寸，实际可访问边界由 mm->brk_current 单独控制
     * （vmm_vma_get 对 VMA_HEAP 有 va >= brk_current 即越界的特判），
     * 所以 sys_brk 扩张时只改一个整数，不需要动 VMA 链表。全程懒分配，不 map。 */
    mm->brk_start   = max_end;
    mm->brk_current = max_end;
    vma_t *heap = vmm_vma_create(max_end, max_end + USER_HEAP_MAX,
                                 VMP_R | VMP_W | VMA_HEAP);
    if (!heap)
    {
        return ENO1_NOMORE_MEM;
    }
    vmm_vma_insert(mm, heap);

    return ENO0_NO_ERROR;
}
