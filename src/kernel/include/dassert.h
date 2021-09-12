#ifndef _ASSERT_H_
#define _ASSERT_H_

#include "console.h"

#  define dassert(expr)							\
  ((void) sizeof ((expr) ? 1 : 0), __extension__ ({			\
      if (expr)								\
        ; /* empty */							\
      else								\
        panic("%s\t%s\t%d",#expr,__FILE__,__LINE__);	\
    }))

#endif