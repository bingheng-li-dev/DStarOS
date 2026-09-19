/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _PIPE_H_
#define _PIPE_H_

#include "memtype.h" /* PGSIZE */
#include "sync.h"    /* osslock_t, waitq_t */
#include "types.h"   /* ssize_t, size_t */
#include "pmm.h"     /* pframe_t, pmm_alloc_page, pmm_free_pages */

struct file;
typedef struct file file_t;

/* 管道环形缓冲大小，等于 PIPE_BUF（POSIX 要求 <= PIPE_BUF 的写具备原子性）。
 * syscall.c 的 do_write_locked 本来就按 SYS_RW_BUF_SIZE=PGSIZE 分块，单次
 * f_op->write 携带的字节数天然 <= PIPE_SIZE，这条依赖若以后调大 SYS_RW_BUF_SIZE
 * 需要一并检查。 */
#define PIPE_SIZE PGSIZE
#define PIPE_BUF  PIPE_SIZE

/**
 * 管道核心结构体。一对读/写 file_t 通过各自的 f_private 共享同一个 pipe_t。
 */
typedef struct pipe
{
    osslock_t lock;     /* 保护以下全部字段，也保护两条等待队列 */
    char     *buf;      /* 一页环形缓冲，pmm_alloc_page() 得来 */
    pframe_t *buf_frame; /* buf 对应的原始页帧，释放时精确对应 pmm_free_pages()——
                          * pmm.h 没有 kva 反推 pframe_t* 的函数，必须自己存一份 */
    uint32_t  head;     /* 下一个写入位置（模 PIPE_SIZE） */
    uint32_t  tail;     /* 下一个读出位置（模 PIPE_SIZE） */
    uint32_t  count;    /* 当前有效字节数，0..PIPE_SIZE；用它判满/判空，
                         * 不靠 head==tail（那样满/空无法区分） */
    int       readers;  /* 打开的读端 file 数 */
    int       writers;  /* 打开的写端 file 数 */
    waitq_t   wq_read;  /* 等数据的读者 */
    waitq_t   wq_write; /* 等空间的写者 */
} pipe_t;

/**
 * @brief 从管道读最多 len 字节到 buf
 * @param[in]  file 管道读端 file（f_private 指向 pipe_t）
 * @param[out] buf  接收数据的内核态缓冲（不是用户指针，拷到用户态由调用方的
 *                  syscall 壳负责，本函数内不碰 copy_to_user）
 * @param[in]  len  期望读取的字节数
 * @retval >0 实际读到的字节数；短读合法，不循环补满（POSIX read 语义）
 * @retval 0  管道为空且写端已全部关闭（EOF）
 * @retval -EAGAIN file->f_mode 带 O_NONBLOCK 且管道为空
 * @note 管道为空、仍有写端、且非 O_NONBLOCK 时阻塞在 pipe->wq_read 上，
 *   直到 pipe_write 或 pipe_release（写端）唤醒。
 */
ssize_t pipe_read(file_t *file, void *buf, size_t len);

/**
 * @brief 向管道写入最多 len 字节
 * @param[in] file 管道写端 file（f_private 指向 pipe_t）
 * @param[in] buf  内核态源缓冲（不是用户指针，从用户态拷入由调用方的 syscall
 *                 壳负责，本函数内不碰 copy_from_user）
 * @param[in] len  期望写入的字节数
 * @retval >0 实际写入的字节数；`len > PIPE_SIZE` 时只写一整块 `PIPE_SIZE`
 *   （短写，上层 do_write_locked 会循环续写）
 * @retval ENO22_BROKEN_PIPE 读端已全部关闭
 * @retval -EAGAIN file->f_mode 带 O_NONBLOCK 且空间不足以容纳 need 字节
 * @note `len <= PIPE_BUF` 的写具备 POSIX 原子性——等够整块所需空间才一次性
 *   写入，不会被另一个写者的数据打断、交错。空间不足、仍有读端、且非
 *   O_NONBLOCK 时阻塞在 pipe->wq_write 上，直到 pipe_read 或 pipe_release
 *  （读端）唤醒。
 */
ssize_t pipe_write(file_t *file, const void *buf, size_t len);

/**
 * @brief 创建一对匿名管道 file（读端 + 写端），共享同一个新建的 pipe_t
 * @param[out] rfile 成功时写入新读端 file_t 指针
 * @param[out] wfile 成功时写入新写端 file_t 指针
 * @retval ENO0_NO_ERROR   成功
 * @retval ENO1_NOMORE_MEM pipe_t / 缓冲页 / 两个 file_t 中任一步分配失败；
 *   已分配的部分会被回滚释放，`*rfile`、`*wfile` 均不会被写入半成品指针
 * @details 读端 `f_mode=O_RDONLY`、`f_op=pipe_read_fops`（只填 .read/.close）；
 *   写端 `f_mode=O_WRONLY`、`f_op=pipe_write_fops`（只填 .write/.close）——
 *   两套独立 fops 让 sys_read/sys_write 现成的 `!f_op->read`/`!f_op->write`
 *   检查自动拒绝"从写端 read()"/"从读端 write()"，不需要在 pipe_read/pipe_write
 *   里重复做权限判断。两个 file 的 f_kind 均为 FILE_KIND_PIPE、f_count=1，
 *   pipe_t 的 readers=writers=1。
 */
int pipe_alloc(file_t **rfile, file_t **wfile);

#endif
