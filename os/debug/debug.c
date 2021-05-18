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
    return 0;
}