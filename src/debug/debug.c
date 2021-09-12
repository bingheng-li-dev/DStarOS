#include "sbi.h"
#include "console.h"
#include "kmalloc.h"
#include "sync.h"
#include "pmm.h"

int main(int argc, char **args)
{
    struct debug
    {
        uint64_t a;
        uint64_t b;
        uint64_t c;
        uint64_t d;
        uint64_t e;
        uint64_t f;
        uint64_t g;
    };
    uint64_t coreid = getCoreId();
    coreid = coreid;
    struct debug *test;
    printf("main::sizeof(struct debug):%ld\n", sizeof(struct debug));
    test = kmalloc(sizeof(struct debug));
    test->a = 1;
    test->b = 2;
    printf("main::test:%08lx,test->a:%ld,core:%ld\n", (phyAddr_t)test, test->a, coreid);
    struct debug *test1;
    test1 = kmalloc(sizeof(struct debug));
    test1->a = 3;
    test1->b = 4;
    printf("main::test1:%08lx,test1->a:%ld,core:%ld\n", (phyAddr_t)test1, test1->a, coreid);
    kfree(test1);
    struct debug *test2;
    printf("main::sizeof(struct debug):%ld\n", sizeof(struct debug));
    test2 = kmalloc(sizeof(struct debug));
    test2->a = 1;
    test2->b = 2;
    printf("main::test2:%08lx,test2->a:%ld,core:%ld\n", (phyAddr_t)test2, test2->a, coreid);
    struct debug *test3;
    test3 = kmalloc(sizeof(struct debug));
    test3->a = 3;
    test3->b = 4;
    printf("main::test3:%08lx,test3->a:%ld,core:%ld\n", (phyAddr_t)test3, test3->a, coreid);
    kfree(test3);
    return 0;
}