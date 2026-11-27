// SPDX-License-Identifier: MIT
/* Host memory port test controls: counts, the n-th allocation failing once
 * through every allocator, the failure record (first kept, count grows,
 * reading keeps the evidence), and the host's diagnostics answers. */
#include <stdio.h>
#include <string.h>
#include "xiaotai/port/mem.h"
#include "xiaotai/port/sys.h"
#include "xiaotai_mem_host.h"

#define CHECK(condition)                                                                    \
    do {                                                                                    \
        if (!(condition)) {                                                                 \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);   \
            return 1;                                                                       \
        }                                                                                   \
    } while (0)

int main(void)
{
    xiaotai_mem_host_clear();
    xiaotai_mem_failure_t failure;
    xiaotai_mem_failure(&failure);
    CHECK(failure.installed && failure.failures == 0 && !failure.has_first);

    /* Counts: attempts and outstanding blocks. */
    void *a = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, 16);
    void *b = xiaotai_mem_calloc(XIAOTAI_MEM_INTERNAL, 2, 8);
    void *c = xiaotai_mem_aligned(XIAOTAI_MEM_DMA, 16, 32);
    CHECK(a && b && c);
    CHECK(xiaotai_mem_host_allocations() == 3 && xiaotai_mem_host_outstanding() == 3);
    xiaotai_mem_free(a);
    xiaotai_mem_free(NULL);
    CHECK(xiaotai_mem_host_outstanding() == 2);

    /* The second allocation from now fails, once, whichever allocator it is. */
    xiaotai_mem_host_fail_nth(2);
    CHECK(xiaotai_mem_host_fail_pending());
    a = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, 16);
    CHECK(a != NULL);
    CHECK(xiaotai_mem_calloc(XIAOTAI_MEM_BULK, 4, 10) == NULL);
    CHECK(!xiaotai_mem_host_fail_pending());
    void *d = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, 16);
    CHECK(d != NULL);
    xiaotai_mem_failure(&failure);
    CHECK(failure.failures == 1 && failure.has_first && failure.requested_size == 40);
    CHECK(strcmp(failure.function, "xiaotai_mem_calloc") == 0);

    /* A later failure counts but never replaces the first record; reading
     * twice gives the same evidence. */
    xiaotai_mem_host_fail_nth(1);
    CHECK(xiaotai_mem_aligned(XIAOTAI_MEM_BULK, 16, 64) == NULL);
    xiaotai_mem_failure(&failure);
    CHECK(failure.failures == 2 && failure.requested_size == 40);
    xiaotai_mem_failure(&failure);
    CHECK(failure.failures == 2 && strcmp(failure.function, "xiaotai_mem_calloc") == 0);

    /* 0 cancels a pending injection. */
    xiaotai_mem_host_fail_nth(1);
    xiaotai_mem_host_fail_nth(0);
    void *e = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, 8);
    CHECK(e != NULL);
    xiaotai_mem_free(a);
    xiaotai_mem_free(b);
    xiaotai_mem_free(c);
    xiaotai_mem_free(d);
    xiaotai_mem_free(e);
    CHECK(xiaotai_mem_host_outstanding() == 0);
    xiaotai_mem_host_clear();
    xiaotai_mem_failure(&failure);
    CHECK(failure.installed && failure.failures == 0 && xiaotai_mem_host_allocations() == 0);

    /* The host has no heap tracing, no task list, no probes and no report lines. */
    CHECK(xiaotai_mem_trace_start() == XIAOTAI_ERR_NOT_SUPPORTED);
    xiaotai_task_info_t tasks[2];
    size_t count = 99;
    CHECK(xiaotai_task_list(tasks, 2, &count) == XIAOTAI_ERR_NOT_SUPPORTED && count == 0);
    CHECK(xiaotai_task_list(NULL, 2, &count) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_port_probes()[0] == NULL);
    CHECK(xiaotai_port_probe("usb_tick", NULL, NULL) == XIAOTAI_ERR_NOT_FOUND);

    printf("PASS: mem counts, injected failures, failure record, host diagnostics\n");
    return 0;
}
