#include "stringops.h"
#include "uaccess.h"

int copy_from_user(void *kdst, const void *usrc, uint64_t n) /* SUM=1 → 直接读用户指针 */
{
    memcpy(kdst, usrc, n);
    return 0;
}

int copy_to_user(void *udst, const void *ksrc, uint64_t n) /* SUM=1 → 直接写用户指针 */
{
    memcpy(udst, ksrc, n);
    return 0;
}
