#ifndef _UACCESS_H_
#define _UACCESS_H_

#include <stdint.h>

int copy_from_user(void *kdst, const void *usrc, uint64_t n);
int copy_to_user(void *udst, const void *ksrc, uint64_t n);

#endif /* _UACCESS_H_ */
