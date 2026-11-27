// SPDX-License-Identifier: MIT
/* The platform core is part of the build: before a restart into provisioning
 * its gate is closed and drained, so no request is in flight when the device
 * restarts (the binding module cancels a running attempt on the quiesce). */
#include "internal.h"
#include "xiaotai/platform.h"

bool prov_gate_present(void) { return true; }

xiaotai_err_t prov_gate_close(uint32_t *ticket)
{
    return xiaotai_platform_gate_close(ticket);
}

bool prov_gate_drained(uint32_t ticket)
{
    return xiaotai_platform_gate_drained(ticket);
}

void prov_gate_open(uint32_t ticket)
{
    (void)xiaotai_platform_gate_open(ticket);
}
