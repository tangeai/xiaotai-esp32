// SPDX-License-Identifier: MIT
#include "../esp-idf/mem_diag.c"
#include <assert.h>

int main(void)
{
    assert(xiaotai_mem_trace_start() == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(xiaotai_mem_trace_stop() == XIAOTAI_ERR_NOT_SUPPORTED);
    assert(xiaotai_mem_trace_dump(NULL, NULL) == XIAOTAI_ERR_NOT_SUPPORTED);
    xiaotai_mem_failure_t state;
    xiaotai_mem_failure(&state);
    assert(!state.installed && !state.failures);
}
