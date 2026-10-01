/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _UACCESS_H_
#define _UACCESS_H_

#include <stdint.h>
#include <stddef.h>
#include "encoding.h"

/* 本内核用户地址空间的简化布局：用户栈固定顶在这里（见 proc.c create_user_mm /
 * run_user_program），往下是用户程序的全部合法虚拟地址。内核高半区地址远高于此
 * （KERNEL_VA_OFFSET = 0xffffffc000000000），所以拿它当一条最粗糙的用户指针范围
 * 检查——挡住"误传内核指针"这类最蠢的越界，不是完备的地址空间校验（真正的校验
 * 需要 page-fault fixup，目前未做）。 */
#define USER_STACK_TOP  0x40000000UL

/* 用户地址空间布局（见 doc/虚拟内存管理.md）：
 *
 *   0x00010000  ELF text/data/bss（user.ld 定死入口）
 *   brk_start   堆，向上长，上限 brk_start + USER_HEAP_MAX
 *   0x20000000  USER_MMAP_BASE，mmap 区，向上长
 *   0x3fff0000  用户栈底（固定 64 KB，不自动增长）
 *   0x40000000  USER_STACK_TOP
 *
 * 堆与 mmap 区之间留一大段空洞，"堆撞上 mmap 区"因此是编译期就不可能发生的事，
 * sys_brk 不需要运行时检查（下面的 _Static_assert 把这条钉死）。 */
#define USER_HEAP_MAX   0x1000000UL     /* 16 MB */
#define USER_MMAP_BASE  0x20000000UL

_Static_assert(0x10000UL + USER_HEAP_MAX < USER_MMAP_BASE,
               "heap must not reach the mmap area");

/**
 * @brief 置位 sstatus.SUM，允许内核访问用户页
 * @return 置位之前的 SUM 位，交给 user_access_end() 恢复，可嵌套
 * @note trap 入口会清 SUM，内核默认不能访问用户页；直接读写用户地址必须包在这一对里。
 */
static inline unsigned long user_access_begin(void)
{
    return set_csr(sstatus, SSTATUS_SUM) & SSTATUS_SUM;
}

/**
 * @brief 恢复 user_access_begin() 之前的 sstatus.SUM
 */
static inline void user_access_end(unsigned long prev)
{
    if (!prev)
    {
        clear_csr(sstatus, SSTATUS_SUM);
    }
}

int copy_from_user(void *kdst, const void *usrc, uint64_t n);
int copy_to_user(void *udst, const void *ksrc, uint64_t n);

/**
 * @brief 从用户空间拷贝一个以 '\0' 结尾的字符串到内核缓冲区。
 * @param[out] kdst 内核目标缓冲区，至少 n 字节。
 * @param[in]  usrc 用户空间源指针。
 * @param[in]  n    kdst 的容量（含 '\0' 的空间）。
 * @retval >=0 成功拷贝的字符串长度（不含 '\0'）。
 * @retval ENO8_NULL_POINTER usrc 为 NULL、越过 USER_STACK_TOP，或某次逐字节拷贝失败。
 * @retval ENO11_NAME_TOO_LONG 在 n 字节内没有遇到 '\0'（调用方按 ENAMETOOLONG 处理）。
 * @note 出于内核栈只有 1 页 4KB 的限制，逐字节拷贝，不在栈上开大缓冲。
 */
long strncpy_from_user(char *kdst, const char *usrc, size_t n);

#endif /* _UACCESS_H_ */
