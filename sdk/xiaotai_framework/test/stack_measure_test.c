// SPDX-License-Identifier: MIT
#include "host_stack.h"

#include <assert.h>
#include <stdbool.h>

#if defined(__SANITIZE_ADDRESS__)
#error "Stack measurement must not run in the sanitizer build"
#endif

static void no_work(void *ctx) { *(bool *)ctx = true; }

static void touch(void *ctx)
{
    volatile unsigned char data[2048];
    for (size_t i = 0; i < sizeof(data); ++i) data[i] = (unsigned char)i;
    bool did_run = false;
    size_t inner = 0;
    assert(!xiaotai_host_stack_measure(no_work, &did_run, &inner));
    *(bool *)ctx = !did_run && data[1] == 1;
}

int main(void)
{
    bool did_run = false;
    size_t idle = 0, used = 0;
    assert(!xiaotai_host_stack_measure(NULL, NULL, &idle));
    assert(xiaotai_host_stack_measure(no_work, &did_run, &idle));
    assert(did_run && idle > 0 && idle < 2048);
    did_run = false;
    assert(xiaotai_host_stack_measure(touch, &did_run, &used));
    /* A stack-use sample includes the host ucontext implementation. */
    assert(did_run && used >= 2048 && used > idle);
    return 0;
}
