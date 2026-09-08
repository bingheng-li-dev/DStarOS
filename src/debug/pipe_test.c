/**
 * @file pipe_test.c
 * @brief 管道核心读写逻辑回归测试（pipe_read/pipe_write/pipe_alloc/pipe_release/
 *   proc_fd_close_on_exec）
 *
 * @details 由 run_sched_tests()（sched_test.c）调用。前五条用例手工构造
 *   pipe_t + 伪造的读端 file_t，绕过分配/写入路径，只验证 pipe_read 本身；
 *   其余用例改用真实的 pipe_alloc() 端到端验证 pipe_write、原子写、环形缓冲
 *   跨边界、阻塞/唤醒、以及 pipe_release 的 EOF/EPIPE 语义；最后一条验证
 *   proc_fd_close_on_exec 对管道 fd 的处理——真实 syscall（sys_pipe2 及
 *   `sys_lseek`/`sys_fstat` 等周边适配）另有覆盖，这里内核态直接调用是
 *   最贴近真实使用的验证方式。
 */

#include "console.h"
#include "proc.h"
#include "sched.h"
#include "sync.h"
#include "list.h"
#include "vfs.h"
#include "pipe.h"
#include "errorcode.h"
#include "stringops.h"
#include "kmalloc.h"
#include "linux_abi.h" /* FD_CLOEXEC */

/* SMP 下等待另一个 hart 上的 reader 到达某个阶段时，主动让出的最大次数上限 */
#define PIPE_YIELD_SPINS 10000

extern void sched_test_check(const char *name, int cond);

/* 手工初始化一个 pipe_t：buf 用 kmalloc(PIPE_SIZE)，测试场景不追求走
 * pmm_alloc_page（那是整页分配器的事，与 pipe_read 本身的逻辑无关），语义等价。 */
static void pipe_test_init(pipe_t *p, int writers)
{
    memset(p, 0, sizeof(*p));
    spinlock_init(&p->lock);
    waitq_init(&p->wq_read);
    waitq_init(&p->wq_write);
    p->buf = kmalloc(PIPE_SIZE);
    p->readers = 1;
    p->writers = writers;
}

/* ============================================================
 * 测试一：有数据可读 —— 基本路径 + tail/count 正确更新
 * ============================================================ */
static void pipe_read_basic_test(void)
{
    printf("\n-- pipe: pipe_read basic (data available) --\n");

    pipe_t p;
    pipe_test_init(&p, 1);

    const char *msg = "hello pipe";
    size_t msglen = strlen(msg);
    memcpy(p.buf, msg, msglen);
    p.head = (uint32_t)msglen;
    p.count = (uint32_t)msglen;

    file_t f;
    memset(&f, 0, sizeof(f));
    f.f_private = &p;
    f.f_mode = O_RDONLY;

    char out[32];
    memset(out, 0, sizeof(out));
    ssize_t r = pipe_read(&f, out, sizeof(out));

    sched_test_check("pipe_read returns full message length", r == (ssize_t)msglen);
    sched_test_check("pipe_read content matches", memcmp(out, msg, msglen) == 0);
    sched_test_check("pipe_read drains count to 0", p.count == 0);
    sched_test_check("pipe_read advances tail", p.tail == (uint32_t)msglen);
}

/* ============================================================
 * 测试二：写端已全部关闭 —— 空管道返回 EOF（0），而非阻塞
 * ============================================================ */
static void pipe_read_eof_test(void)
{
    printf("\n-- pipe: pipe_read returns EOF when writers==0 --\n");

    pipe_t p;
    pipe_test_init(&p, 0); /* writers=0, count=0 */

    file_t f;
    memset(&f, 0, sizeof(f));
    f.f_private = &p;
    f.f_mode = O_RDONLY;

    char out[4];
    ssize_t r = pipe_read(&f, out, sizeof(out));
    sched_test_check("pipe_read returns 0 (EOF) when writers==0 and empty", r == 0);
}

/* ============================================================
 * 测试三：O_NONBLOCK 空读 —— 返回 -EAGAIN，不阻塞
 * ============================================================ */
static void pipe_read_nonblock_test(void)
{
    printf("\n-- pipe: pipe_read returns -EAGAIN on O_NONBLOCK empty read --\n");

    pipe_t p;
    pipe_test_init(&p, 1); /* writers=1, count=0：换非 NONBLOCK 会阻塞，这里不能阻塞 */

    file_t f;
    memset(&f, 0, sizeof(f));
    f.f_private = &p;
    f.f_mode = O_RDONLY | O_NONBLOCK;

    char out[4];
    ssize_t r = pipe_read(&f, out, sizeof(out));
    sched_test_check("pipe_read returns -EAGAIN on nonblocking empty read", r == -EAGAIN);
}

/* ============================================================
 * 测试四：环形缓冲跨边界读取 —— 两段 memcpy 都要生效
 * ============================================================ */
static void pipe_read_wraparound_test(void)
{
    printf("\n-- pipe: pipe_read across ring buffer wraparound --\n");

    pipe_t p;
    pipe_test_init(&p, 1);

    /* tail 设在离缓冲区末端只剩 3 字节的位置，数据横跨边界：
     * 前 3 字节落在 [PIPE_SIZE-3, PIPE_SIZE)，后 3 字节落在 [0, 3) */
    const char *msg = "ABCDEF";
    size_t msglen = 6;
    uint32_t start = PIPE_SIZE - 3;
    memcpy(p.buf + start, msg, 3);
    memcpy(p.buf, msg + 3, 3);
    p.tail = start;
    p.count = (uint32_t)msglen;

    file_t f;
    memset(&f, 0, sizeof(f));
    f.f_private = &p;
    f.f_mode = O_RDONLY;

    char out[8];
    memset(out, 0, sizeof(out));
    ssize_t r = pipe_read(&f, out, msglen);

    sched_test_check("wraparound read returns full length", r == (ssize_t)msglen);
    sched_test_check("wraparound read content matches", memcmp(out, msg, msglen) == 0);
    sched_test_check("wraparound read wraps tail correctly", p.tail == 3);
    sched_test_check("wraparound read drains count", p.count == 0);
}

/* ============================================================
 * 测试五：管道为空、仍有写端、非 O_NONBLOCK —— 阻塞直到被唤醒
 * ============================================================ */
static pipe_t pipe_block_p;
static file_t pipe_block_f;
static volatile int pipe_block_stage; /* 0=未开始 1=已挂入 wq_read 阻塞 2=读到数据返回 */
static char pipe_block_result[32];
static ssize_t pipe_block_ret;

static void *pipe_block_reader(void *arg)
{
    (void)arg;
    pipe_block_stage = 1;
    ssize_t r = pipe_read(&pipe_block_f, pipe_block_result, sizeof(pipe_block_result));
    pipe_block_ret = r;
    pipe_block_stage = 2;
    return NULL;
}

static void pipe_read_block_wakeup_test(void)
{
    printf("\n-- pipe: pipe_read blocks on empty pipe, wakes on data --\n");

    pipe_test_init(&pipe_block_p, 1);
    memset(&pipe_block_f, 0, sizeof(pipe_block_f));
    pipe_block_f.f_private = &pipe_block_p;
    pipe_block_f.f_mode = O_RDONLY;
    pipe_block_stage = 0;

    create_kernel_thread_by_fork(pipe_block_reader, NULL, 0);

    /* 等 reader 真正阻塞：轮询 wq_read.task_list 非空，而不是轮询 stage——
     * stage=1 只代表"即将调用 pipe_read"，不代表已经挂进等待队列 */
    int blocked = 0;
    for (int spin = 0; spin < PIPE_YIELD_SPINS; spin++)
    {
        sched_schedule();
        irq_key_t pipe_block_p_lock_key = spinlock_acquire(&pipe_block_p.lock);
        blocked = !list_empty(&pipe_block_p.wq_read.task_list);
        spinlock_release(&pipe_block_p.lock, pipe_block_p_lock_key);
        if (blocked)
        {
            break;
        }
    }
    sched_test_check("reader blocked on empty pipe (queued on wq_read)", blocked);

    /* 模拟写者直接灌数据（这里手工操作字段，不经 pipe_write）+ 唤醒 */
    const char *msg = "wakeup!";
    size_t msglen = strlen(msg);
    irq_key_t pipe_block_p_lock_key = spinlock_acquire(&pipe_block_p.lock);
    memcpy(pipe_block_p.buf, msg, msglen);
    pipe_block_p.head = (uint32_t)msglen;
    pipe_block_p.count = (uint32_t)msglen;
    waitq_wake_all(&pipe_block_p.wq_read);
    spinlock_release(&pipe_block_p.lock, pipe_block_p_lock_key);

    int status = 0;
    int16_t c = do_wait(-1, &status, 0);
    sched_test_check("reader reaped after wakeup", c > 0);
    sched_test_check("reader got correct length", pipe_block_ret == (ssize_t)msglen);
    sched_test_check("reader got correct content", memcmp(pipe_block_result, msg, msglen) == 0);
}

/* ============================================================
 * 测试六：pipe_write 基本路径 —— 走真实 pipe_alloc，写入后 pipe_read 读回
 * ============================================================ */
static void pipe_write_basic_test(void)
{
    printf("\n-- pipe: pipe_write basic + pipe_read round-trip (via pipe_alloc) --\n");

    file_t *rf = NULL, *wf = NULL;
    int ret = pipe_alloc(&rf, &wf);
    sched_test_check("pipe_alloc succeeds", ret == ENO0_NO_ERROR);

    const char *msg = "round trip";
    size_t msglen = strlen(msg);
    ssize_t w = pipe_write(wf, msg, msglen);
    sched_test_check("pipe_write returns full length", w == (ssize_t)msglen);

    char out[32];
    memset(out, 0, sizeof(out));
    ssize_t r = pipe_read(rf, out, sizeof(out));
    sched_test_check("pipe_read gets what was written",
                      r == (ssize_t)msglen && memcmp(out, msg, msglen) == 0);

    /* 收尾：两端都关闭（走 vfs_close，与未来 proc_fd_close 的真实路径一致），
     * 无法直接断言 pipe_t 内存已释放，但至少验证两次关闭都不崩溃、返回成功 */
    sched_test_check("vfs_close read end ok", vfs_close(rf) == ENO0_NO_ERROR);
    sched_test_check("vfs_close write end ok", vfs_close(wf) == ENO0_NO_ERROR);
}

/* ============================================================
 * 测试七：读端已全部关闭 —— 写入返回 EPIPE
 * ============================================================ */
static void pipe_write_epipe_test(void)
{
    printf("\n-- pipe: pipe_write returns EPIPE when readers==0 --\n");

    file_t *rf = NULL, *wf = NULL;
    pipe_alloc(&rf, &wf);

    vfs_close(rf); /* 关闭读端：readers 归零 */

    ssize_t w = pipe_write(wf, "x", 1);
    sched_test_check("pipe_write returns ENO22_BROKEN_PIPE when readers==0",
                      w == ENO22_BROKEN_PIPE);

    vfs_close(wf);
}

/* ============================================================
 * 测试八：O_NONBLOCK 写满的管道 —— 返回 -EAGAIN；顺带验证 PIPE_SIZE 整块原子写
 * ============================================================ */
static void pipe_write_nonblock_full_test(void)
{
    printf("\n-- pipe: pipe_write returns -EAGAIN when O_NONBLOCK and pipe full --\n");

    file_t *rf = NULL, *wf = NULL;
    pipe_alloc(&rf, &wf);
    wf->f_mode |= O_NONBLOCK;

    char *big = kmalloc(PIPE_SIZE);
    memset(big, 'A', PIPE_SIZE);
    ssize_t w1 = pipe_write(wf, big, PIPE_SIZE);
    sched_test_check("first write fills pipe exactly (atomic PIPE_SIZE write)",
                      w1 == (ssize_t)PIPE_SIZE);

    ssize_t w2 = pipe_write(wf, "x", 1);
    sched_test_check("second write on full pipe returns -EAGAIN", w2 == -EAGAIN);

    kfree(big);
    vfs_close(rf);
    vfs_close(wf);
}

/* ============================================================
 * 测试九：pipe_write 环形缓冲跨边界写入 —— 用真实 写/读 把 head/tail 推到
 * 离缓冲区末端只剩 3 字节，再验证跨界写入能被跨界读回
 * ============================================================ */
static void pipe_write_wraparound_test(void)
{
    printf("\n-- pipe: pipe_write across ring buffer wraparound --\n");

    file_t *rf = NULL, *wf = NULL;
    pipe_alloc(&rf, &wf);

    char *filler = kmalloc(PIPE_SIZE);
    char *drain = kmalloc(PIPE_SIZE);
    memset(filler, 'Z', PIPE_SIZE - 3);
    pipe_write(wf, filler, PIPE_SIZE - 3);
    pipe_read(rf, drain, PIPE_SIZE - 3);
    kfree(filler);
    kfree(drain);

    pipe_t *p = (pipe_t *)wf->f_private;
    sched_test_check("head positioned near buffer end", p->head == PIPE_SIZE - 3);
    sched_test_check("tail positioned near buffer end", p->tail == PIPE_SIZE - 3);

    const char *msg = "ABCDEF"; /* 6 字节，必然跨越缓冲区末端 */
    size_t msglen = 6;
    ssize_t w = pipe_write(wf, msg, msglen);
    sched_test_check("wraparound write returns full length", w == (ssize_t)msglen);

    char out[8];
    memset(out, 0, sizeof(out));
    ssize_t r = pipe_read(rf, out, msglen);
    sched_test_check("wraparound write content read back correctly",
                      r == (ssize_t)msglen && memcmp(out, msg, msglen) == 0);

    vfs_close(rf);
    vfs_close(wf);
}

/* ============================================================
 * 测试十：管道写满、非 O_NONBLOCK —— 阻塞直到读者排空后被唤醒完成写入
 * ============================================================ */
static file_t *pipe_wblock_rf;
static file_t *pipe_wblock_wf;
static ssize_t pipe_wblock_ret;

static void *pipe_wblock_writer(void *arg)
{
    (void)arg;
    char *big = kmalloc(PIPE_SIZE);
    memset(big, 'B', PIPE_SIZE);
    pipe_write(pipe_wblock_wf, big, PIPE_SIZE); /* 管道原本为空，这次不阻塞 */
    ssize_t r = pipe_write(pipe_wblock_wf, "X", 1); /* 管道已满，这次会阻塞 */
    pipe_wblock_ret = r;
    kfree(big);
    return NULL;
}

static void pipe_write_block_wakeup_test(void)
{
    printf("\n-- pipe: pipe_write blocks on full pipe, wakes after reader drains --\n");

    pipe_alloc(&pipe_wblock_rf, &pipe_wblock_wf);

    create_kernel_thread_by_fork(pipe_wblock_writer, NULL, 0);

    /* 等 writer 真正阻塞在第二次写：轮询 wq_write.task_list 非空 */
    pipe_t *p = (pipe_t *)pipe_wblock_wf->f_private;
    int blocked = 0;
    for (int spin = 0; spin < PIPE_YIELD_SPINS; spin++)
    {
        sched_schedule();
        irq_key_t p_lock_key = spinlock_acquire(&p->lock);
        blocked = !list_empty(&p->wq_write.task_list);
        spinlock_release(&p->lock, p_lock_key);
        if (blocked)
        {
            break;
        }
    }
    sched_test_check("writer blocked on full pipe (queued on wq_write)", blocked);

    /* 读者读走 1 字节腾出空间，pipe_read 内部会 waitq_wake_all(&wq_write) */
    char one[1];
    ssize_t rr = pipe_read(pipe_wblock_rf, one, 1);
    sched_test_check("reader drained 1 byte", rr == 1);

    int status = 0;
    int16_t c = do_wait(-1, &status, 0);
    sched_test_check("writer reaped after wakeup", c > 0);
    sched_test_check("writer completed the pending 1-byte write", pipe_wblock_ret == 1);

    vfs_close(pipe_wblock_rf);
    vfs_close(pipe_wblock_wf);
}

/* ============================================================
 * 测试十一：关闭写端 —— 阻塞的读者被唤醒，返回 EOF（0）
 * ============================================================ */
static file_t *pipe_rel1_rf;
static file_t *pipe_rel1_wf;
static ssize_t pipe_rel1_ret;

static void *pipe_rel1_reader(void *arg)
{
    (void)arg;
    char buf[4];
    ssize_t r = pipe_read(pipe_rel1_rf, buf, sizeof(buf));
    pipe_rel1_ret = r;
    return NULL;
}

static void pipe_release_write_then_read_eof_test(void)
{
    printf("\n-- pipe: closing write end wakes blocked reader with EOF --\n");

    pipe_alloc(&pipe_rel1_rf, &pipe_rel1_wf);

    create_kernel_thread_by_fork(pipe_rel1_reader, NULL, 0);

    pipe_t *p = (pipe_t *)pipe_rel1_rf->f_private;
    int blocked = 0;
    for (int spin = 0; spin < PIPE_YIELD_SPINS; spin++)
    {
        sched_schedule();
        irq_key_t p_lock_key = spinlock_acquire(&p->lock);
        blocked = !list_empty(&p->wq_read.task_list);
        spinlock_release(&p->lock, p_lock_key);
        if (blocked)
        {
            break;
        }
    }
    sched_test_check("reader blocked on empty pipe before write end closes", blocked);

    vfs_close(pipe_rel1_wf); /* 关闭写端：writers 归零，应唤醒阻塞读者 */

    int status = 0;
    int16_t c = do_wait(-1, &status, 0);
    sched_test_check("reader reaped after write end closed", c > 0);
    sched_test_check("reader got EOF (0) after write end closed", pipe_rel1_ret == 0);

    vfs_close(pipe_rel1_rf);
}

/* ============================================================
 * 测试十二：关闭读端 —— 阻塞的写者被唤醒，返回 EPIPE
 * ============================================================ */
static file_t *pipe_rel2_rf;
static file_t *pipe_rel2_wf;
static ssize_t pipe_rel2_ret;

static void *pipe_rel2_writer(void *arg)
{
    (void)arg;
    char *big = kmalloc(PIPE_SIZE);
    memset(big, 'C', PIPE_SIZE);
    pipe_write(pipe_rel2_wf, big, PIPE_SIZE); /* 填满，不阻塞 */
    ssize_t r = pipe_write(pipe_rel2_wf, "Y", 1); /* 管道已满，会阻塞 */
    pipe_rel2_ret = r;
    kfree(big);
    return NULL;
}

static void pipe_release_read_then_write_epipe_test(void)
{
    printf("\n-- pipe: closing read end wakes blocked writer with EPIPE --\n");

    pipe_alloc(&pipe_rel2_rf, &pipe_rel2_wf);

    create_kernel_thread_by_fork(pipe_rel2_writer, NULL, 0);

    pipe_t *p = (pipe_t *)pipe_rel2_wf->f_private;
    int blocked = 0;
    for (int spin = 0; spin < PIPE_YIELD_SPINS; spin++)
    {
        sched_schedule();
        irq_key_t p_lock_key = spinlock_acquire(&p->lock);
        blocked = !list_empty(&p->wq_write.task_list);
        spinlock_release(&p->lock, p_lock_key);
        if (blocked)
        {
            break;
        }
    }
    sched_test_check("writer blocked on full pipe before read end closes", blocked);

    vfs_close(pipe_rel2_rf); /* 关闭读端：readers 归零，应唤醒阻塞写者 */

    int status = 0;
    int16_t c = do_wait(-1, &status, 0);
    sched_test_check("writer reaped after read end closed", c > 0);
    sched_test_check("writer got EPIPE after read end closed",
                      pipe_rel2_ret == ENO22_BROKEN_PIPE);

    vfs_close(pipe_rel2_wf);
}

/* ============================================================
 * 测试十三：proc_fd_close_on_exec 只关带 FD_CLOEXEC 的管道 fd，
 * 且底层 pipe_t 引用计数正确联动——对端能感知到关闭
 * ============================================================ */
static void pipe_close_on_exec_test(void)
{
    printf("\n-- pipe: proc_fd_close_on_exec closes CLOEXEC pipe fd, keeps the other --\n");

    file_t *rf = NULL, *wf = NULL;
    pipe_alloc(&rf, &wf);

    int rfd = proc_fd_alloc();
    proc_fd_install(rfd, rf);
    proc_fd_set_flags(rfd, FD_CLOEXEC); /* 读端带 CLOEXEC，写端不带 */

    int wfd = proc_fd_alloc();
    proc_fd_install(wfd, wf);

    proc_fd_close_on_exec(proc_get_current());

    sched_test_check("CLOEXEC pipe fd closed after exec", proc_fd_get(rfd) == NULL);
    sched_test_check("non-CLOEXEC pipe fd survives exec", proc_fd_get(wfd) == wf);

    /* 读端已经被关（readers 归零），写端应该能感知到：写入返回 EPIPE，
     * 证明 proc_fd_close_on_exec 走的是真正的 vfs_close 路径（递减了
     * pipe_t 的 readers），而不是只清空 fd 表槽位、漏调 f_op->close */
    ssize_t w = pipe_write(wf, "x", 1);
    sched_test_check("writer sees EPIPE after CLOEXEC read end closed on exec",
                      w == ENO22_BROKEN_PIPE);

    proc_fd_close(wfd); /* 收尾：关掉剩下的 fd，回收 pipe_t */
}

/* ============================================================
 * 聚合入口：由 run_sched_tests()（sched_test.c）调用
 * ============================================================ */
void run_pipe_tests(void)
{
    pipe_read_basic_test();
    pipe_read_eof_test();
    pipe_read_nonblock_test();
    pipe_read_wraparound_test();
    pipe_read_block_wakeup_test();
    pipe_write_basic_test();
    pipe_write_epipe_test();
    pipe_write_nonblock_full_test();
    pipe_write_wraparound_test();
    pipe_write_block_wakeup_test();
    pipe_release_write_then_read_eof_test();
    pipe_release_read_then_write_epipe_test();
    pipe_close_on_exec_test();
}
