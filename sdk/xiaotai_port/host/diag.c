// SPDX-License-Identifier: MIT
/* Port diagnostics on the host: no chip-level facts, no probes, and no task
 * list (threads have no stack watermark the port could read). */
#include "xiaotai/port/sys.h"

void xiaotai_port_report(xiaotai_port_write_fn_t write, void *ctx)
{
    (void)write;
    (void)ctx;
}

xiaotai_err_t xiaotai_port_probe(const char *name, xiaotai_port_write_fn_t write, void *ctx)
{
    (void)name;
    (void)write;
    (void)ctx;
    return XIAOTAI_ERR_NOT_FOUND;
}

const char *const *xiaotai_port_probes(void)
{
    static const char *const none[] = {NULL};
    return none;
}

xiaotai_err_t xiaotai_task_list(xiaotai_task_info_t *tasks, size_t capacity, size_t *count)
{
    if (!count || (capacity && !tasks)) return XIAOTAI_ERR_INVALID_ARG;
    *count = 0;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}
