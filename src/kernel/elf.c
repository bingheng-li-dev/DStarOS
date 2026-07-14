#include "elf.h"
#include "console.h"
#include "stringops.h"
#include "errorcode.h"

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

int elf_load(mm_t *mm, const unsigned char *image, uint64_t size, virAddr_t *entry)
{
    Elf64_Ehdr *ehdr = (Elf64_Ehdr *)image; /* 文件头在image最开头 */

    /* 最小校验：ELF 来源可能是磁盘/未来 execve 传入的用户文件，不可信，
     * 因此返回错误码交由调用者（如未来的 do_exec）决定如何处理。
     * 不做 p_vaddr 范围 / p_align 校验：那是地址空间隔离问题，与本函数的内存安全无关；
     * 但 size 边界必须校验——一旦 image 来自磁盘，越界的 e_phoff/p_offset/p_filesz
     * 会导致读取 image 缓冲区之外的内核内存。 */
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

    /* 程序头表在 e_phoff 处；PT_LOAD 段的内容在 image + phdr[i].p_offset 处 */
    Elf64_Phdr *phdr_base = (Elf64_Phdr *)(image + ehdr->e_phoff);
    /* 遍历 e_phnum 个 Elf64_Phdr，处理 PT_LOAD 可加载段
     * 注意：调用者需保证各 PT_LOAD 段按页对齐、互不共享物理页（见 lds/user.ld 的段间对齐），
     * 本函数按段独立建 VMA 并映射，不处理多个段共享同一物理页的场景。 */
    for (int i = 0; i < ehdr->e_phnum; i++)
    {
        Elf64_Phdr *ph = &phdr_base[i];
        if (ph->p_type != PT_LOAD)
        {
            continue; /* 只处理加载段 */
        }

        /* 段内容必须落在 [0, size) 内，否则下面的 memcpy 会读到 image 缓冲区之外 */
        if (ph->p_offset > size || ph->p_filesz > size - ph->p_offset)
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

        /* 转换段权限 PF_R/PF_W/PF_X → mm管理器pgprot_t标记
         * 注意：这里始终额外加上 VMP_W，而不是严格按 p_flags 来。
         * 原因：紧接着的 memcpy/memset 要把文件内容写进这段刚映射好的页——
         * SUM 只豁免 U/S 特权位检查，不豁免 PTE 本身的 R/W/X 位，如果段本身是
         * 只读/只读可执行（没有 PF_W，例如常见的 .text 段），硬件会在这次写入时
         * 直接触发 store page fault。项目目前没有"先可写、拷完再改回只读"的
         * 重新映射原语（vmm.c 的 get_pte 未导出），所以暂时统一放宽为可写，
         * 不严格区分 R+X 与 R+W+X 段。代价是用户代码段本身也能被自己改写
         * （无 W^X 隔离）；这是里程碑 1B 的已知简化，非安全加固场景不需要现在补。 */
        pgprot_t flag = VMP_W;
        if (ph->p_flags & PF_R)
        {
            flag |= VMP_R;
        }
        if (ph->p_flags & PF_X)
        {
            flag |= VMP_X;
        }

        /* 分配虚拟内存：p_vaddr 目标虚拟地址，p_memsz 内存总大小（含bss） */
        vma_t *vma = vmm_vma_create(
            round_down_page(ph->p_vaddr), round_up_page(ph->p_vaddr + ph->p_memsz), flag
        );
        if (!vma)
        {
            return ENO1_NOMORE_MEM;
        }
        vmm_vma_insert(mm, vma);
        if (vmm_map_vma(mm, vma) != ENO0_NO_ERROR)
        {
            return ENO1_NOMORE_MEM;
        }

        /* 拷贝文件中存在的数据；SUM 已开，且 satp 已切到该 mm，可直接写用户 VA */
        memcpy((void *)ph->p_vaddr, image + ph->p_offset, ph->p_filesz);
        /* 零填充bss区：内存大小 > 文件大小的部分清零 */
        memset((void *)ph->p_vaddr + ph->p_filesz, 0, ph->p_memsz - ph->p_filesz);
    }
    *entry = ehdr->e_entry; /* 输出程序入口地址 */

    return ENO0_NO_ERROR;
}
