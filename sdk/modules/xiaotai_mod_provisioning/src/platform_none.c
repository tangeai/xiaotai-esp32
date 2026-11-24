// SPDX-License-Identifier: MIT
/* Without the platform core there are no platform requests to drain: a
 * restart into provisioning closes no gate. */
#include "internal.h"

bool prov_gate_present(void) { return false; }

xiaotai_err_t prov_gate_close(uint32_t *ticket)
{
    if (ticket) *ticket = 0;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

bool prov_gate_drained(uint32_t ticket)
{
    (void)ticket;
    return true;
}

void prov_gate_open(uint32_t ticket) { (void)ticket; }
