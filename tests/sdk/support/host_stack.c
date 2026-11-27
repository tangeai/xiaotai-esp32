// SPDX-License-Identifier: MIT
#include "host_stack.h"

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#define STACK_BYTES (64U * 1024U)
#define FILL_BYTE 0xA5U

typedef struct {
    void (*run)(void *);
    void *ctx;
    bool returned;
} measurement_t;

static _Thread_local measurement_t *current;

static void measure_entry(void)
{
    current->run(current->ctx);
    current->returned = true;
}

bool xiaotai_host_stack_measure(void (*run)(void *), void *ctx, size_t *used)
{
    if (!run || !used || current) return false;
    const long system_page = sysconf(_SC_PAGESIZE);
    if (system_page <= 0 || STACK_BYTES % (size_t)system_page != 0) return false;
    const size_t page = (size_t)system_page;
    const size_t length = STACK_BYTES + 2 * page;
    unsigned char *mapping = mmap(NULL, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mapping == MAP_FAILED) return false;
    unsigned char *stack = mapping + page;
    ucontext_t caller, alternate;
    measurement_t measurement = {.run = run, .ctx = ctx};
    bool ok = false;
    if (mprotect(stack, STACK_BYTES, PROT_READ | PROT_WRITE) == 0 && getcontext(&alternate) == 0) {
        memset(stack, FILL_BYTE, STACK_BYTES);
        alternate.uc_stack = (stack_t){.ss_sp = stack, .ss_size = STACK_BYTES};
        alternate.uc_link = &caller;
        makecontext(&alternate, measure_entry, 0);
        current = &measurement;
        ok = swapcontext(&caller, &alternate) == 0 && measurement.returned;
        current = NULL;
        if (ok) {
            /* Supported POSIX hosts use a downward-growing stack. */
            size_t low = 0;
            while (low < STACK_BYTES && stack[low] == FILL_BYTE) ++low;
            *used = STACK_BYTES - low;
        }
    }
    (void)munmap(mapping, length);
    return ok;
}
