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

void tty_input_push(char c)
{
    irq_key_t g_tty_lock_key = spinlock_acquire(&g_tty.lock);

    /* 1. ICRNL：串口送来的 \r 当作行结束，放最前面，后面所有判断只需认 \n */
    if (c == '\r' && (g_tty.tio.c_iflag & ICRNL))
    {
        c = '\n';
    }

    /* 2. raw 模式短路：每个字符自成一行，后面的特殊键处理全部跳过——
     * ^U/退格在 raw 模式下是普通字节，交给用户程序自己解释。 */
    if (!(g_tty.tio.c_lflag & ICANON))
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
        spinlock_release(&g_tty.lock, g_tty_lock_key);
        return;
    }

    /* 3. VINTR（^C）与 VQUIT（^\），仅当 ISIG 打开：丢弃整个未提交半行，
     * 并给前台进程组发信号。**这里是中断上下文、手里还攥着 tty->lock**，所以只能走
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
        spinlock_release(&g_tty.lock, g_tty_lock_key);
        return;
    }

    /* 4. VKILL（^U）：杀掉整个未提交半行（简化：不做逐字符退格的擦除序列） */
    if (c == (char)g_tty.tio.c_cc[VKILL])
    {
        g_tty.edit_pos = g_tty.line_pos;
        if (g_tty.tio.c_lflag & ECHOK)
        {
            tty_echo('\n');
        }
        spinlock_release(&g_tty.lock, g_tty_lock_key);
        return;
    }

    /* 5. VERASE（DEL 0x7F 或 ^H 0x08，不同终端送的不一样，都要认）：退一格。
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
        spinlock_release(&g_tty.lock, g_tty_lock_key);
        return;
    }

    /* 6. VEOF（^D）：字节本身不入缓冲、不回显。
     * 行首（edit_pos == line_pos，当前行还没敲任何字符）按下：把这个位置存进
     * eof_queue，交给 tty_read 在 read_pos 追到队首那个位置时返回 0。队列满则丢弃
     * （响铃）——一批喂入里挂起这么多个从未被读走的 ^D 已经是极端情况，不值得为此
     * 无限扩容。
     * 行中（已敲了字符）按下：立即提交这半行（line_pos = edit_pos），读者醒来正常
     * 拿到这些字节——不碰 eof_queue：里面排队的都是更早发生、位置更靠前的 ^D，
     * 依然有效，必须留着等 read_pos 依次追上去才交付，不能因为这里提交了新的一行
     * 就把它们冲掉。 */
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
        spinlock_release(&g_tty.lock, g_tty_lock_key);
        return;
    }

    /* 7. 普通字符：缓冲满则丢弃并响铃；否则入队，\n 触发整行提交并唤醒读者 */
    if (g_tty.edit_pos - g_tty.read_pos >= TTY_BUF_SIZE)
    {
        tty_echo('\a');
        spinlock_release(&g_tty.lock, g_tty_lock_key);
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
    spinlock_release(&g_tty.lock, g_tty_lock_key);
}

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
        /* 被 tty_input_push 唤醒后从这里继续，回到循环开头重新检查条件——
         * 可能被虚假唤醒，或数据已被抢先取走，不能想当然直接成功 */
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

    /* 用 read_pos == eof_queue[0]（而不是 read_pos == line_pos）判断 EOF 是否该在此刻
     * 交付：队首记录的是最早那个 ^D 生效时的确切位置，即便此后又提交了更多行
     * （line_pos 前移，甚至排进了更多 EOF），这个位置本身没变——必须先把 read_pos
     * 推到这里交付一次 EOF，再继续读后面的数据，不能因为"眼下 line_pos 已经比
     * read_pos 大很多"就跳过它。 */
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

    /* 队首若还有一个未交付的 EOF 落在本次可读范围内（read_pos < eof_queue[0] <
     * read_pos+n，不是上面已经处理过的 read_pos == eof_queue[0] 那种情况），必须
     * 把这次读取截断在它之前——EOF 只能单独作为下一次 read() 的返回值（0 字节）
     * 交付，不能被这次的数据"跨过去"，否则读者会在该看到 EOF 的地方直接读到
     * EOF 之后才提交的数据。 */
    if (tty->eof_count > 0)
    {
        uint32_t eof_off = tty->eof_queue[0] - tty->read_pos;
        if (eof_off < n)
        {
            n = eof_off;
        }
    }

    /* canonical 模式下 [read_pos, line_pos) 可能已经攒了不止一行（读者迟迟不来读，
     * 期间敲了好几个回车）——一次 read 只能吐一行，找到第一个 \n 就截断在那里，
     * 哪怕 len/avail 还有富余；没有 \n 的情况（行中 ^D 提交的半行）保持不截断。
     * raw 模式没有这个限制，line_pos 本来就逐字节推进，不需要额外扫描。 */
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

/* devfs 挂载 /dev/console、/dev/tty 时，vfs_open() 走通用路径构造 file_t，
 * 会无条件把 f_kind 设成 FILE_KIND_VFS（见 vfs.c）——必须在 f_op->open 回调里
 * 改回来，否则 sys_read/sys_write 会误把 vfs_big_lock 套在会阻塞的 tty_read()
 * 外面，等于把 Phase 4 刚拆掉的"持锁睡眠"问题在设备文件上重新引入一遍。 */
static int tty_vfs_open(inode_t *inode, file_t *file, int mode)
{
    UNUSED(inode);
    UNUSED(mode);
    file->f_kind = FILE_KIND_DEVICE;
    file->f_private = &g_tty;
    return ENO0_NO_ERROR;
}

/* 非 static：devfs.c 的 /dev/console、/dev/tty 两个 inode 直接复用这张表
 * （而不是自己另建一张内容相同的表）——tty_from_file() 靠 f_op == &tty_fops
 * 的指针比对判断"这是不是真正的 TTY"，两条构造路径（tty_open_file() 直接
 * kmalloc，或 devfs 挂载后走 vfs_open()）必须共享同一张表，指针比对才能
 * 覆盖两条路径。 */
file_operations_t tty_fops = {
    .read = tty_read,
    .write = tty_write,
    .open = tty_vfs_open,
};

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

tty_t *tty_from_file(file_t *f)
{
    if (f == NULL || f->f_kind != FILE_KIND_DEVICE || f->f_op != &tty_fops)
    {
        return NULL;
    }
    return (tty_t *)f->f_private;
}

void tty_set_foreground_pgid(int16_t pgid)
{
    irq_key_t key = spinlock_acquire(&g_tty.lock);
    g_tty.foreground_pgid = pgid;
    spinlock_release(&g_tty.lock, key);
}

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
