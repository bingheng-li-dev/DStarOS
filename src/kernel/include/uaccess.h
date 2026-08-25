#ifndef _UACCESS_H_
#define _UACCESS_H_

#include <stdint.h>
#include <stddef.h>

/* 本内核用户地址空间的简化布局：用户栈固定顶在这里（见 proc.c create_user_mm /
 * run_user_program），往下是用户程序的全部合法虚拟地址。内核高半区地址远高于此
 * （KERNEL_VA_OFFSET = 0xffffffc000000000），所以拿它当一条最粗糙的用户指针范围
 * 检查——挡住"误传内核指针"这类最蠢的越界，不是完备的地址空间校验（真正的校验
 * 需要 page-fault fixup，目前未做）。 */
#define USER_STACK_TOP  0x40000000UL

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
