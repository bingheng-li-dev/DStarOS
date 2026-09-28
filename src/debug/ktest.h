/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Copyright (C) 2021-2026 BingHeng Li <bingheng-li@outlook.com> */

#ifndef _KTEST_H_
#define _KTEST_H_

#include "console.h"

/* 一套内核态自检的断言计数；tag 用在失败行与收尾标记里 */
typedef struct
{
    const char *tag;
    int pass;
    int fail;
} ktest_t;

/* 每条都打印："  [PASS] name" / "  [FAIL] name" */
static inline void ktest_check(ktest_t *t, const char *name, int cond)
{
    if (cond)
    {
        printf("  [PASS] %s\n", name);
        t->pass++;
    }
    else
    {
        printf("  [FAIL] %s\n", name);
        t->fail++;
    }
}

/* 通过时不出声，失败打印 "[tag] FAIL: what" */
static inline void ktest_expect(ktest_t *t, int cond, const char *what)
{
    if (cond)
    {
        t->pass++;
    }
    else
    {
        t->fail++;
        printf("[%s] FAIL: %s\n", t->tag, what);
    }
}

static inline void ktest_reset(ktest_t *t)
{
    t->pass = 0;
    t->fail = 0;
}

/* 收尾标记 "=== tag done: P pass  F fail ==="，scripts/regress.sh 以它判一套跑完 */
static inline void ktest_done(const ktest_t *t)
{
    printf("=== %s done: %d pass  %d fail ===\n", t->tag, t->pass, t->fail);
}

#endif /* _KTEST_H_ */
