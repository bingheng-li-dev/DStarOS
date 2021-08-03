#include "sbi.h"
#include "tinyprintf.h"
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
    struct debug *test;
    printf("main::sizeof(struct debug):%ld\n", sizeof(struct debug));
    test = kmalloc(sizeof(struct debug));
    test->a = 1;
    test->b = 2;
    printf("main::test:%08lx,test->a:%ld,test->b:%ld\n", (phyAddr_t)test, test->a, test->b);
    kfree(test);
    struct debug *test1;
    test1 = kmalloc(sizeof(struct debug));
    test1->a = 3;
    test1->b = 4;
    printf("main::test1:%08lx,test1->a:%ld,test1->b:%ld\n", (phyAddr_t)test1, test1->a, test1->b);
    pframe_t *lala = convert_pa2pframe_flr((phyAddr_t)kmalloc(PGSIZE));
    printf("main::lala:%08lx\n", (phyAddr_t)convert_pframe2pa(lala));
    kfree(lala);
    struct debug *test3;
    test3 = kmalloc(PGSIZE);
    test3->e = 5;
    test3->f = 6;
    printf("main::test3:%08lx,test3->e:%ld,test3->f:%ld\n", (phyAddr_t)test3, test3->e, test3->f);
    kfree(test3);
    struct debug *test4;
    test4 = kmalloc(PGSIZE*2);
    test4->e = 5;
    test4->f = 6;
    printf("main::test4:%08lx,test4->e:%ld,test4->f:%ld\n", (phyAddr_t)test4, test4->e, test4->f);
    // kfree(test4);
    struct debug *test5;
    test5 = kmalloc(PGSIZE);
    test5->e = 5;
    test5->f = 6;
    printf("main::test5:%08lx,test5->e:%ld,test5->f:%ld\n", (phyAddr_t)test5, test5->e, test5->f);
    kfree(test5);
    return 0;
}