#include "sbi.h"
#include "console.h"
#include "syscall.h"
#include "uaccess.h"

static long sys_write(int fd, const char *ubuf, uint64_t len)
{
    if (fd != 1 && fd != 2)
    {
        return -1; /* 1A：仅 stdout/stderr 到串口 */
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
            return -1;
        }
        for (uint64_t i = 0; i < n; i++)
        {
            sbi_console_putchar((int)kbuf[i]);
        }

        done += n;
    }
    return (long)len;
}

static long sys_exit(int code)
{
    printf("[user] exit(%d)\n", code);
    sbi_shutdown(); /* 1A 单进程里程碑：直接关机；Phase 2 改为 zombie+wait */
    return 0;       /* unreachable */
}

long syscall_dispatch(intstkf_t *sp)
{
    switch (sp->x17_a7) /* a7中存放了系统调用号 */
    {
    case __NR_write:
        return sys_write((int)sp->x10_a0, (const char *)sp->x11_a1, sp->x12_a2);
    case __NR_exit:
        return sys_exit((int)sp->x10_a0);
    default:
        printf("syscall: unknown nr=%ld\n", sp->x17_a7);
        return -1;
    }
}
