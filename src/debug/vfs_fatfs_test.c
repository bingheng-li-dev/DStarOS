#include "sbi.h"
#include "console.h"
#include "kmalloc.h"
#include "sync.h"
#include "pmm.h"
#include "vfs.h"
#include "errorcode.h"
#include "stringops.h"

static int vfs_pass = 0;
static int vfs_fail = 0;

static void check(const char *name, int cond)
{
    if (cond) {
        printf("  [PASS] %s\n", name);
        vfs_pass++;
    } else {
        printf("  [FAIL] %s\n", name);
        vfs_fail++;
    }
}

void vfs_test(void)
{
    printf("\n=== VFS basic test ===\n");

    /* ---- write + read roundtrip ---- */
    file_t *f = vfs_open("/hello.txt", O_RDWR | O_CREAT);
    check("open /hello.txt O_CREAT", f != NULL);
    if (f) {
        ssize_t w = vfs_write(f, "hello DStarOS", 13);
        check("write 13 bytes", w == 13);
        vfs_close(f);
    }

    char buf[32];
    memset(buf, 0, sizeof(buf));
    f = vfs_open("/hello.txt", O_RDONLY);
    check("open /hello.txt O_RDONLY", f != NULL);
    if (f) {
        ssize_t r = vfs_read(f, buf, 13);
        check("read 13 bytes", r == 13);
        check("read content correct", strncmp(buf, "hello DStarOS", 13) == 0);
        vfs_close(f);
    }

    /* ---- stat file ---- */
    stat_t st;
    int ret = vfs_stat("/hello.txt", &st);
    check("stat /hello.txt", ret == ENO0_NO_ERROR);
    check("stat: is regular file", S_ISREG(st.st_mode));
    check("stat: size == 13", st.st_size == 13);

    /* ---- mkdir + stat dir ---- */
    ret = vfs_mkdir("/testdir", 0755);
    check("mkdir /testdir", ret == ENO0_NO_ERROR);
    ret = vfs_stat("/testdir", &st);
    check("stat /testdir", ret == ENO0_NO_ERROR);
    check("stat: is directory", S_ISDIR(st.st_mode));

    /* ---- file in subdir ---- */
    f = vfs_open("/testdir/sub.txt", O_RDWR | O_CREAT);
    check("open /testdir/sub.txt O_CREAT", f != NULL);
    if (f) {
        ssize_t w = vfs_write(f, "sub", 3);
        check("write 3 bytes to subdir file", w == 3);
        vfs_close(f);
    }

    /* ---- truncate ---- */
    ret = vfs_truncate("/hello.txt", 5);
    check("truncate /hello.txt to 5", ret == ENO0_NO_ERROR);
    ret = vfs_stat("/hello.txt", &st);
    check("stat after truncate: size == 5", st.st_size == 5);

    /* ---- rename ---- */
    ret = vfs_rename("/hello.txt", "/renamed.txt");
    check("rename /hello.txt -> /renamed.txt", ret == ENO0_NO_ERROR);
    ret = vfs_stat("/renamed.txt", &st);
    check("stat /renamed.txt after rename", ret == ENO0_NO_ERROR);
    ret = vfs_stat("/hello.txt", &st);
    check("stat old name -> NOSUCH", ret == ENO5_NOSUCH_ENTRY);

    /* ---- unlink ---- */
    ret = vfs_unlink("/renamed.txt");
    check("unlink /renamed.txt", ret == ENO0_NO_ERROR);
    ret = vfs_stat("/renamed.txt", &st);
    check("stat deleted file -> NOSUCH", ret == ENO5_NOSUCH_ENTRY);

    /* ---- rmdir (must be empty first) ---- */
    ret = vfs_unlink("/testdir/sub.txt");
    check("unlink /testdir/sub.txt", ret == ENO0_NO_ERROR);
    ret = vfs_rmdir("/testdir");
    check("rmdir /testdir", ret == ENO0_NO_ERROR);
    ret = vfs_stat("/testdir", &st);
    check("stat deleted dir -> NOSUCH", ret == ENO5_NOSUCH_ENTRY);

    printf("=== VFS test done: %d pass  %d fail ===\n\n", vfs_pass, vfs_fail);
}
