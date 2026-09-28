/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#include "tty.h"
#include "signal.h"
#include "proc.h"
#include "slab.h"
#include "stringops.h"
#include "sbi.h"
#include "uart.h"
#include "sched.h"
#include "console.h"
#include "kmalloc.h"
#include "vfs.h"
#include "errorcode.h"

#define UNUSED(x) (void)(x)

/* 全系统唯一的 TTY 单例：一个串口即一个终端，不做多终端/pty。只在 tty.c 内部访问——
 * fd 0/1/2 的 file_t.f_private 指向本结构体（由 tty_open_file() 建立），
 * 外部一律经 file_t 间接访问。 */
static tty_t g_tty;

/**
 * @brief 初始化全局 TTY 单例：自旋锁 + 等待队列 + termios 默认值
 */
void tty_init(void)
{
    spinlock_init(&g_tty.lock);
    waitq_init(&g_tty.wq_read);

    g_tty.read_pos = 0;
    g_tty.line_pos = 0;
    g_tty.edit_pos = 0;
    g_tty.eof_count = 0;

    /* termios 默认值：canonical 模式 + 回显 + 信号识别，ICRNL 保证串口送来的 \r
     * 被当作行结束（不开的话敲回车没反应）。
     * 未显式赋值的 c_cc 下标（VSWTC/VEOL/VREPRINT/VDISCARD/VWERASE/VLNEXT/VEOL2，
     * 本内核不实现对应功能）保持 memset 清零的 0，即"禁用"。 */
    memset(&g_tty.tio, 0, sizeof(g_tty.tio));
    g_tty.tio.c_iflag = ICRNL;
    g_tty.tio.c_oflag = OPOST | ONLCR;
    g_tty.tio.c_cflag = 0;
    g_tty.tio.c_lflag = ISIG | ICANON | ECHO | ECHOE | ECHOK;
    g_tty.tio.c_cc[VINTR]  = 3;   /* ^C */
    g_tty.tio.c_cc[VQUIT]  = 28;  /* ^\ */
    g_tty.tio.c_cc[VERASE] = 127; /* DEL */
    g_tty.tio.c_cc[VKILL]  = 21;  /* ^U */
    g_tty.tio.c_cc[VEOF]   = 4;   /* ^D */
    g_tty.tio.c_cc[VTIME]  = 0;
    g_tty.tio.c_cc[VMIN]   = 1;

    g_tty.tio.c_cc[VSTART] = 17;  /* ^Q，IXON 未开启，当前不生效 */
    g_tty.tio.c_cc[VSTOP]  = 19;  /* ^S，同上 */
    g_tty.tio.c_cc[VSUSP]  = 26;  /* ^Z，无 job control，当前不生效 */

    /* 前台进程组初值取 init 的 pid；第一个用户进程建好时由 run_user_program 覆盖成
     * 它自己的组，再往后由 ioctl(TIOCSPGRP) 接管 */
    g_tty.foreground_pgid = 1;

    g_tty.ws.ws_row = 24;
    g_tty.ws.ws_col = 80;
    g_tty.ws.ws_xpixel = 0;
    g_tty.ws.ws_ypixel = 0;
}

/* 回显一个字符。直接调 uart_putc，不碰 ConsoleLock（printf/tty_write 持的锁）
 * ——两者交叉嵌套会引入一条新锁序，极端情况下回显字符可能插进一条 printf 中间，
 * 交互场景下可接受。ONLCR 的 \n -> \r\n 在这里做。 */
static void tty_echo(char c)
{
    if (c == '\n' && (g_tty.tio.c_oflag & OPOST) && (g_tty.tio.c_oflag & ONLCR))
    {
        uart_putc('\r');
    }
    uart_putc(c);
}

static void tty_echo_str(const char *s)
{
    for (; *s; s++)
    {
        tty_echo(*s);
    }
}

/* raw 模式：每个字符自成一行，特殊键处理全部跳过——^U/退格在 raw 模式下是普通字节，
 * 交给用户程序自己解释。以下 tty_input_* 均由 tty_input_push 持 g_tty.lock 调用。 */
static void tty_input_raw(char c)
{
    if (g_tty.edit_pos - g_tty.read_pos < TTY_BUF_SIZE)
    {
        g_tty.buf[g_tty.edit_pos % TTY_BUF_SIZE] = c;
        g_tty.edit_pos++;
        g_tty.line_pos = g_tty.edit_pos;
        if (g_tty.tio.c_lflag & ECHO)
        {
            tty_echo(c);
        }
        waitq_wake_all(&g_tty.wq_read);
    }
    else
    {
        tty_echo('\a');
    }
}

/* canonical 模式的特殊键，处理了返回 true */
static bool tty_input_special(char c)
{
    /* VINTR（^C）与 VQUIT（^\），仅当 ISIG 打开：丢弃整个未提交半行，
     * 并给前台进程组发信号。这里是中断上下文、手里还攥着 tty->lock，所以只能走
     * signal_send_group 这条"置位 + 唤醒"的路径；真正的投递发生在目标自己返回 U 态
     * 那一刻（最迟一个 tick 之后）。锁序 tty->lock → proc_list_lock → sighand->lock
     * → run_queue.lock 单向成立：信号侧全程不碰 tty。 */
    if ((g_tty.tio.c_lflag & ISIG) &&
        (c == (char)g_tty.tio.c_cc[VINTR] || c == (char)g_tty.tio.c_cc[VQUIT]))
    {
        bool is_intr = (c == (char)g_tty.tio.c_cc[VINTR]);
        g_tty.edit_pos = g_tty.line_pos;
        tty_echo_str(is_intr ? "^C\n" : "^\\\n");
        signal_send_group(g_tty.foreground_pgid, is_intr ? SIGINT : SIGQUIT);
        waitq_wake_all(&g_tty.wq_read);
        return true;
    }

    /* VKILL（^U）：杀掉整个未提交半行（简化：不做逐字符退格的擦除序列） */
    if (c == (char)g_tty.tio.c_cc[VKILL])
    {
        g_tty.edit_pos = g_tty.line_pos;
        if (g_tty.tio.c_lflag & ECHOK)
        {
            tty_echo('\n');
        }
        return true;
    }

    /* VERASE（DEL 0x7F 或 ^H 0x08，不同终端送的不一样，都要认）：退一格。
     * 只能退到 line_pos，不能把已提交的行退回来、更不能退到已被读走的字节。 */
    if (c == (char)g_tty.tio.c_cc[VERASE] || c == 0x08)
    {
        if (g_tty.edit_pos > g_tty.line_pos)
        {
            g_tty.edit_pos--;
            if (g_tty.tio.c_lflag & ECHOE)
            {
                tty_echo_str("\b \b");
            }
        }
        return true;
    }

    /* VEOF（^D）：字节本身不入缓冲、不回显。
     * 行首按下：把位置存进 eof_queue，tty_read 在 read_pos 追到它时返回 0；队列满则响铃丢弃。
     * 行中按下：立即提交这半行，不动 eof_queue——里面是更早的 ^D，仍要依次交付。 */
    if (c == (char)g_tty.tio.c_cc[VEOF])
    {
        if (g_tty.edit_pos == g_tty.line_pos)
        {
            if (g_tty.eof_count < TTY_MAX_PENDING_EOF)
            {
                g_tty.eof_queue[g_tty.eof_count++] = g_tty.edit_pos;
            }
            else
            {
                tty_echo('\a');
            }
        }
        else
        {
            g_tty.line_pos = g_tty.edit_pos;
        }
        waitq_wake_all(&g_tty.wq_read);
        return true;
    }
    return false;
}

/* canonical 模式的普通字符：缓冲满则丢弃并响铃；否则入队，\n 触发整行提交并唤醒读者 */
static void tty_input_char(char c)
{
    if (g_tty.edit_pos - g_tty.read_pos >= TTY_BUF_SIZE)
    {
        tty_echo('\a');
        return;
    }
    g_tty.buf[g_tty.edit_pos % TTY_BUF_SIZE] = c;
    g_tty.edit_pos++;
    if (g_tty.tio.c_lflag & ECHO)
    {
        tty_echo(c);
    }
    if (c == '\n')
    {
        g_tty.line_pos = g_tty.edit_pos;
        waitq_wake_all(&g_tty.wq_read);
    }
}

/**
 * @brief 行规范层：把一个从串口读到的字符喂给 TTY
 */
void tty_input_push(char c)
{
    irq_key_t g_tty_lock_key = spinlock_acquire(&g_tty.lock);

    /* ICRNL：串口送来的 \r 当作行结束，放最前面，后面所有判断只需认 \n */
    if (c == '\r' && (g_tty.tio.c_iflag & ICRNL))
    {
        c = '\n';
    }

    if (!(g_tty.tio.c_lflag & ICANON))
    {
        tty_input_raw(c);
    }
    else if (!tty_input_special(c))
    {
        tty_input_char(c);
    }

    spinlock_release(&g_tty.lock, g_tty_lock_key);
}

/**
 * @brief TTY 读端：阻塞直到有一整行可读（raw 模式下至少一个字节）
 */
ssize_t tty_read(file_t *file, void *buf, size_t len)
{
    tty_t *tty = (tty_t *)file->f_private;

    irq_key_t tty_lock_key = spinlock_acquire(&tty->lock);
    while (tty->read_pos == tty->line_pos &&
           !(tty->eof_count > 0 && tty->read_pos == tty->eof_queue[0]))
    {
        if (file->f_mode & O_NONBLOCK)
        {
            spinlock_release(&tty->lock, tty_lock_key);
            return -EAGAIN;
        }
        waitq_prepare_interruptible(&tty->wq_read);
        /* 同 pipe_read：这次检查必须夹在"置 INTERRUPTIBLE"和"睡下去"之间，
         * 否则 ^C 的 SIGINT 有一个窗口会丢掉唤醒，读者就此睡死 */
        if (signal_pending(proc_get_current()))
        {
            waitq_remove(&tty->wq_read, proc_get_current());
            proc_get_current()->proc_state = RUNNING;
            spinlock_release(&tty->lock, tty_lock_key);
            return ENO24_RESTARTSYS;
        }
        spinlock_release(&tty->lock, tty_lock_key);
        /* 被唤醒不代表条件成立（数据可能已被抢先取走），回循环重检 */
        sched_schedule();
        /* 重新取锁：赋值给循环外的 key，不能再声明一个同名局部把它遮蔽掉 */
        tty_lock_key = spinlock_acquire(&tty->lock);

        /* ^C 打进来的 SIGINT 走的正是这条路：既要把睡在这里的读者唤醒，
         * 也要让它带着 -ERESTARTSYS 退出去，交给投递点处理 */
        if (signal_pending(proc_get_current()))
        {
            waitq_remove(&tty->wq_read, proc_get_current());
            spinlock_release(&tty->lock, tty_lock_key);
            return ENO24_RESTARTSYS;
        }
    }

    /* 队首记的是最早那个 ^D 生效时的确切位置：read_pos 追到它就先交付一次 EOF，
     * 不能因为此后又提交了更多行（line_pos 已前移）就跳过它。 */
    if (tty->eof_count > 0 && tty->read_pos == tty->eof_queue[0])
    {
        tty->eof_count--;
        for (size_t i = 0; i < tty->eof_count; i++)
        {
            tty->eof_queue[i] = tty->eof_queue[i + 1];
        }
        spinlock_release(&tty->lock, tty_lock_key);
        return 0;
    }

    size_t n = len;
    size_t avail = tty->line_pos - tty->read_pos;
    if (n > avail)
    {
        n = avail;
    }

    /* 队首 EOF 落在本次可读范围内时截断在它之前：EOF 只能作为下一次 read() 单独交付，
     * 不能被这次的数据跨过去。 */
    if (tty->eof_count > 0)
    {
        uint32_t eof_off = tty->eof_queue[0] - tty->read_pos;
        if (eof_off < n)
        {
            n = eof_off;
        }
    }

    /* canonical 模式下 [read_pos, line_pos) 可能攒了不止一行，一次 read 只吐一行：
     * 截断在第一个 \n 处；没有 \n（行中 ^D 提交的半行）则不截断。raw 模式不扫描。 */
    if (tty->tio.c_lflag & ICANON)
    {
        for (size_t i = 0; i < n; i++)
        {
            if (tty->buf[(tty->read_pos + i) % TTY_BUF_SIZE] == '\n')
            {
                n = i + 1;
                break;
            }
        }
    }

    for (size_t i = 0; i < n; i++)
    {
        ((char *)buf)[i] = tty->buf[(tty->read_pos + i) % TTY_BUF_SIZE];
    }
    tty->read_pos += (uint32_t)n;

    spinlock_release(&tty->lock, tty_lock_key);
    return (ssize_t)n; /* 短读合法（POSIX read 语义）*/
}

/**
 * @brief TTY 写端：逐字节输出，按 OPOST|ONLCR 做 \n -> \r\n
 */
ssize_t tty_write(file_t *file, const void *buf, size_t len)
{
    tty_t *tty = (tty_t *)file->f_private;

    /* 持 ConsoleLock（不持 tty->lock），保证整段用户输出不被 printf 打断；
     * termios 里的 c_oflag 只读一次问题不大——写的过程中被 ioctl 并发改掉是
     * 罕见场景，不值得为此再抢一次 tty->lock。 */
    irq_key_t ConsoleLock_key = spinlock_acquire(&ConsoleLock);
    for (size_t i = 0; i < len; i++)
    {
        char c = ((const char *)buf)[i];
        if (c == '\n' && (tty->tio.c_oflag & OPOST) && (tty->tio.c_oflag & ONLCR))
        {
            uart_putc('\r');
        }
        uart_putc(c);
    }
    spinlock_release(&ConsoleLock, ConsoleLock_key);

    return (ssize_t)len;
}

/* vfs_open() 把 f_kind 一律设成 FILE_KIND_VFS，这里改回 DEVICE：否则 sys_read /
 * sys_write 会把 vfs_big_lock 套在会阻塞的 tty_read() 外面，持锁睡眠。 */
static int tty_vfs_open(inode_t *inode, file_t *file, int mode)
{
    UNUSED(inode);
    UNUSED(mode);
    file->f_kind = FILE_KIND_DEVICE;
    file->f_private = &g_tty;
    return ENO0_NO_ERROR;
}

/* 非 static：devfs 的 /dev/console、/dev/tty 直接复用，理由见 tty.h */
file_operations_t tty_fops = {
    .read = tty_read,
    .write = tty_write,
    .open = tty_vfs_open,
};

/**
 * @brief 造一个 TTY 设备 file（stdin/stdout/stderr 的后端）
 */
file_t *tty_open_file(void)
{
    file_t *f = slab_cache_alloc(file_cache);
    if (f == NULL)
    {
        return NULL;
    }
    memset(f, 0, sizeof(file_t));
    f->f_count = 1;
    f->f_op = &tty_fops;
    f->f_mode = O_RDWR;
    f->f_kind = FILE_KIND_DEVICE;
    f->f_private = &g_tty;

    return f;
}

/**
 * @brief 若 f 确实是 tty_fops 打开的 file，返回其 tty_t*，否则返回 NULL
 */
tty_t *tty_from_file(file_t *f)
{
    if (f == NULL || f->f_kind != FILE_KIND_DEVICE || f->f_op != &tty_fops)
    {
        return NULL;
    }
    return (tty_t *)f->f_private;
}

/**
 * @brief 设置前台进程组（^C / ^\ 打给它）
 */
void tty_set_foreground_pgid(int16_t pgid)
{
    irq_key_t key = spinlock_acquire(&g_tty.lock);
    g_tty.foreground_pgid = pgid;
    spinlock_release(&g_tty.lock, key);
}

/**
 * @brief 轮询 UART 接收寄存器，把读到的字符逐个喂给 tty_input_push()
 */
void tty_poll_input(void)
{
    /* 两个 hart 都轮询在正确性上没问题（tty_lock 挡住了），但会让字符顺序依赖
     * 锁的公平性、行为不可复现；限定 hart0 让输入路径确定。 */
    if (cpu_get_core_id() != 0)
    {
        return;
    }

    /* 一次 tick 内可能积压多个字符（粘贴、脚本喂入），循环取空而不是只取一个，
     * 否则输入会以 5ms/字符的速度慢慢渗出。上界防止"输入源一直有数据"
     * 把中断处理程序卡住。 */
    for (int n = 0; n < TTY_BUF_SIZE; n++)
    {
        int c = uart_getc();
        if (c == -1)
        {
            break;
        }
        tty_input_push((char)c);
    }
}
