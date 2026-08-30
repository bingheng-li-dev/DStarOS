#include "pipe.h"
#include "slab.h"
#include "vfs.h"
#include "sched.h"
#include "errorcode.h"
#include "stringops.h"
#include "kmalloc.h"

ssize_t pipe_read(file_t *file, void *buf, size_t len)
{
    pipe_t *p = (pipe_t *)file->f_private;

    spinlock_acquire(&p->lock);
    while (p->count == 0)
    {
        if (p->writers == 0)
        {
            spinlock_release(&p->lock);
            return 0; /* 写端已全部关闭：EOF */
        }
        if (file->f_mode & O_NONBLOCK)
        {
            spinlock_release(&p->lock);
            return -EAGAIN;
        }
        waitq_prepare(&p->wq_read);
        spinlock_release(&p->lock);
        /* 被 pipe_write 或 pipe_release（写端）唤醒后从这里继续，回到循环开头
         * 重新检查条件——可能被虚假唤醒或被别的读者抢先取走，不能想当然直接成功 */
        sched_schedule();
        spinlock_acquire(&p->lock);
    }

    size_t n = len;
    if (n > p->count)
    {
        n = p->count;
    }

    /* 从 tail 拷出 n 字节，可能跨越缓冲区末端，分两段 */
    size_t first = PIPE_SIZE - p->tail;
    if (first > n)
    {
        first = n;
    }
    memcpy(buf, p->buf + p->tail, first);
    if (n > first)
    {
        memcpy((char *)buf + first, p->buf, n - first);
    }

    p->tail = (p->tail + (uint32_t)n) % PIPE_SIZE;
    p->count -= (uint32_t)n;

    waitq_wake_all(&p->wq_write);
    spinlock_release(&p->lock);

    return (ssize_t)n; /* 短读合法，不循环补满 */
}

ssize_t pipe_write(file_t *file, const void *buf, size_t len)
{
    pipe_t *p = (pipe_t *)file->f_private;

    spinlock_acquire(&p->lock);
    if (p->readers == 0)
    {
        spinlock_release(&p->lock);
        return ENO22_BROKEN_PIPE; /* TODO：信号机制落地后同时投 SIGPIPE */
    }

    /* <= PIPE_BUF 的写要原子：等够整块所需空间再一次性写入；
     * len > PIPE_SIZE 时只写这一整块，短写交给上层 do_write_locked 循环续写 */
    size_t need = len;
    if (need > PIPE_SIZE)
    {
        need = PIPE_SIZE;
    }

    while (PIPE_SIZE - p->count < need)
    {
        if (p->readers == 0)
        {
            spinlock_release(&p->lock);
            return ENO22_BROKEN_PIPE;
        }
        if (file->f_mode & O_NONBLOCK)
        {
            spinlock_release(&p->lock);
            return -EAGAIN;
        }
        waitq_prepare(&p->wq_write);
        spinlock_release(&p->lock);
        /* 被 pipe_read 或 pipe_release（读端）唤醒后从这里继续，回到循环开头
         * 重新检查条件——可能被虚假唤醒或被别的写者抢先占用了腾出的空间 */
        sched_schedule();
        spinlock_acquire(&p->lock);
    }

    /* 从 head 拷入 need 字节，可能跨越缓冲区末端，分两段 */
    size_t first = PIPE_SIZE - p->head;
    if (first > need)
    {
        first = need;
    }
    memcpy(p->buf + p->head, buf, first);
    if (need > first)
    {
        memcpy(p->buf, (const char *)buf + first, need - first);
    }

    p->head = (p->head + (uint32_t)need) % PIPE_SIZE;
    p->count += (uint32_t)need;

    waitq_wake_all(&p->wq_read);
    spinlock_release(&p->lock);

    return (ssize_t)need;
}

/* pipe_read_close/pipe_write_close 共用的关闭逻辑：is_reader 区分递减
 * readers 还是 writers。关闭本身不改变 count，但改变了对端的"提前退出条件"
 * （readers==0 → EPIPE，writers==0 → EOF），睡着的对端必须被踢起来重新检查，
 * 两条 waitq_wake_all 都不能省。 */
static void pipe_release_common(pipe_t *p, bool is_reader)
{
    spinlock_acquire(&p->lock);
    if (is_reader)
    {
        p->readers--;
        waitq_wake_all(&p->wq_write);
    }
    else
    {
        p->writers--;
        waitq_wake_all(&p->wq_read);
    }
    bool empty = (p->readers == 0 && p->writers == 0);
    spinlock_release(&p->lock);

    if (empty)
    {
        dealloc(p->buf_frame);
        kfree(p);
    }
}

static int pipe_read_close(file_t *file)
{
    pipe_release_common((pipe_t *)file->f_private, true);
    return ENO0_NO_ERROR;
}

static int pipe_write_close(file_t *file)
{
    pipe_release_common((pipe_t *)file->f_private, false);
    return ENO0_NO_ERROR;
}

/* 两套独立 fops：读端只填 .read/.close，写端只填 .write/.close。
 * sys_read/sys_write 现成的 !f_op->read / !f_op->write 检查会自动拒绝
 * "从写端 read()"/"从读端 write()"，不需要在 pipe_read/pipe_write 里
 * 重复做权限判断。 */
static file_operations_t pipe_read_fops = {
    .read = pipe_read,
    .close = pipe_read_close,
};

static file_operations_t pipe_write_fops = {
    .write = pipe_write,
    .close = pipe_write_close,
};

int pipe_alloc(file_t **rfile, file_t **wfile)
{
    pipe_t *p = slab_cache_alloc(pipe_cache);
    if (!p)
    {
        return ENO1_NOMORE_MEM;
    }

    pframe_t *frame = slab_alloc_page_retry();
    if (!frame)
    {
        kfree(p);
        return ENO1_NOMORE_MEM;
    }

    file_t *rf = slab_cache_alloc(file_cache);
    if (!rf)
    {
        dealloc(frame);
        kfree(p);
        return ENO1_NOMORE_MEM;
    }

    file_t *wf = slab_cache_alloc(file_cache);
    if (!wf)
    {
        kfree(rf);
        dealloc(frame);
        kfree(p);
        return ENO1_NOMORE_MEM;
    }

    spinlock_init(&p->lock);
    waitq_init(&p->wq_read);
    waitq_init(&p->wq_write);
    p->buf = (char *)convert_pframe2kva(frame);
    p->buf_frame = frame;
    p->head = 0;
    p->tail = 0;
    p->count = 0;
    p->readers = 1;
    p->writers = 1;

    memset(rf, 0, sizeof(*rf));
    rf->f_op = &pipe_read_fops;
    rf->f_mode = O_RDONLY;
    rf->f_count = 1;
    rf->f_kind = FILE_KIND_PIPE;
    rf->f_private = p;

    memset(wf, 0, sizeof(*wf));
    wf->f_op = &pipe_write_fops;
    wf->f_mode = O_WRONLY;
    wf->f_count = 1;
    wf->f_kind = FILE_KIND_PIPE;
    wf->f_private = p;

    *rfile = rf;
    *wfile = wf;
    return ENO0_NO_ERROR;
}
