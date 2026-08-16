#include "sbi.h"
#include "console.h"
#include "syscall.h"
#include "uaccess.h"
#include "cpu.h"
#include "proc.h"
#include "vfs.h"

static long sys_write(int fd, const char *ubuf, uint64_t len)
{
    file_t *f = proc_fd_get(fd);
    if (!f || !f->f_op || !f->f_op->write)
    {
        return -1;
    }

    char kbuf[256];
    uint64_t done = 0;
    while (done < len)
    {
        uint64_t n = len - done;
        if (n > sizeof(kbuf))
        {
            n = sizeof(kbuf);
        }
        if (copy_from_user(kbuf, ubuf + done, n) != 0)
        {
            return done ? (long)done : -1;
        }
        ssize_t w = f->f_op->write(f, kbuf, n);
        if (w < 0)
        {
            return done ? (long)done : -1;
        }
        done += (uint64_t)w;
        if ((uint64_t)w < n) /* 短写：底层没接收完，停 */
        {
            break;
        }
    }
    return (long)done;
}

static long sys_read(int fd, char *ubuf, uint64_t len)
{
    file_t *f = proc_fd_get(fd);
    if (!f || !f->f_op || !f->f_op->read)
    {
        return -1;
    }

    char kbuf[256];
    if (len > sizeof(kbuf))
    {
        len = sizeof(kbuf);
    }
    ssize_t r = f->f_op->read(f, kbuf, len);
    if (r < 0)
    {
        return -1;
    }
    if (r > 0 && copy_to_user(ubuf, kbuf, (uint64_t)r) != 0)
    {
        return -1;
    }
    return (long)r;
}

static long sys_close(int fd)
{
    return proc_fd_close(fd);
}

/* dup(oldfd)：把 oldfd 复制到当前进程最小空闲 fd，两者指向同一个 file_t（共享偏移）*/
static long sys_dup(int oldfd)
{
    file_t *f = proc_fd_get(oldfd);
    if (!f)
    {
        return -1; /* oldfd 无效 */
    }
    int newfd = proc_fd_alloc();
    if (newfd < 0)
    {
        return -1; /* fd 表已满 */
    }
    f->f_count++;
    proc_fd_install(newfd, f);
    return newfd;
}

/* dup3(oldfd, newfd, flags)：把 oldfd 复制到指定的 newfd（若已打开则先关掉）。
 * flags 里的 O_CLOEXEC 暂不支持（无 exec fd 表清理逻辑）。用户态 dup2 = dup3(o,n,0)，
 * 且 dup2 在库层处理 oldfd==newfd，故内核 dup3 对相等直接判非法。 */
static long sys_dup3(int oldfd, int newfd, int flags)
{
    (void)flags;
    file_t *f = proc_fd_get(oldfd);
    if (!f)
    {
        return -1; /* oldfd 无效 */
    }
    if (oldfd == newfd || newfd < 0 || newfd >= NOFILE)
    {
        return -1; /* dup3 要求 oldfd != newfd；newfd 越界非法 */
    }
    if (proc_fd_get(newfd))
    {
        /* newfd 已占用：先关闭。oldfd 仍持有 f 的引用，即便二者是同一 file_t 也不会被提前释放 */
        proc_fd_close(newfd);
    }
    f->f_count++;
    proc_fd_install(newfd, f);
    return newfd;
}

static long sys_exit(int code)
{
    do_exit((int16_t)code);
    return 0; /* unreachable */
}

static long sys_getpid(void)
{
    return proc_get_current()->proc_pid;
}

static long sys_getppid(void)
{
    pcb_t *p = proc_get_current()->proc_parent;
    return p ? p->proc_pid : 0;
}

static long sys_clone(intstkf_t *sp)
{
    return do_fork(0, sp->x2_sp, sp);
}

/* a0=path, a1=argv, a2=envp（本阶段忽略 argv/envp）。成功后 sp 已被改写为进入新程序的帧，
 * 本函数返回 0；trap.c 会把 0 写回 a0（新程序 _start 不读 argc，无害）。失败返回负 ENO*。 */
static long sys_execve(intstkf_t *sp)
{
    return do_exec(sp, (const char *)sp->x10_a0);
}

static long sys_wait4(int pid, int *ustatus, int options, void *rusage)
{
    int kstatus = 0;
    int16_t ret = do_wait((int16_t)pid, &kstatus);
    if (ret > 0 && ustatus)
    {
        if (copy_to_user(ustatus, &kstatus, sizeof(kstatus)) != 0)
        {
            return -1;
        }
    }
    
    return ret;
}

long syscall_dispatch(intstkf_t *sp)
{
    switch (sp->x17_a7) /* a7中存放了系统调用号 */
    {
    case __NR_write:
        return sys_write((int)sp->x10_a0, (const char *)sp->x11_a1, sp->x12_a2);
    case __NR_read:
        return sys_read((int)sp->x10_a0, (char *)sp->x11_a1, sp->x12_a2);
    case __NR_close:
        return sys_close((int)sp->x10_a0);
    case __NR_dup:
        return sys_dup((int)sp->x10_a0);
    case __NR_dup3:
        return sys_dup3((int)sp->x10_a0, (int)sp->x11_a1, (int)sp->x12_a2);
    case __NR_exit:
    case __NR_exit_group: /* 本阶段暂作 exit 别名，不区分线程组 */
        return sys_exit((int)sp->x10_a0);
    case __NR_getpid:
        return sys_getpid();
    case __NR_getppid:
        return sys_getppid();
    case __NR_clone:
        return sys_clone(sp);
    case __NR_execve:
        return sys_execve(sp);
    case __NR_wait4:
        return sys_wait4((int)sp->x10_a0, (int *)sp->x11_a1, (int)sp->x12_a2, (void *)sp->x13_a3);
    default:
        printf("syscall: unknown nr=%ld\n", sp->x17_a7);
        return -1;
    }
}
