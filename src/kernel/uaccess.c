#include "stringops.h"
#include "uaccess.h"
#include "errorcode.h"

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

long strncpy_from_user(char *kdst, const char *usrc, size_t n)
{
    if (usrc == NULL || (uint64_t)usrc >= USER_STACK_TOP)
    {
        return ENO8_NULL_POINTER;
    }

    size_t i;
    for (i = 0; i < n; i++)
    {
        if (copy_from_user(&kdst[i], usrc + i, 1) != 0)
        {
            return ENO8_NULL_POINTER;
        }
        if (kdst[i] == '\0')
        {
            return (long)i;
        }
    }
    return ENO11_NAME_TOO_LONG;
}
