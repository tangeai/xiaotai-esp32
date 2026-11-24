// SPDX-License-Identifier: MIT
/* Fake of the platform core's maintenance gate (xiaotai/platform.h), linked
 * into the tests built with src/with_platform.c only: the test without the
 * platform core proves the module links without any platform symbol. */
#include "harness.h"
#include "xiaotai/platform.h"

fake_gate_t fake_gate;

bool fake_gate_is_closed_and_drained(void) { return fake_gate.closed && fake_gate.drained; }

xiaotai_err_t xiaotai_platform_gate_close(uint32_t *ticket)
{
    ++fake_gate.closes;
    if (fake_gate.close_result != XIAOTAI_OK) return fake_gate.close_result;
    if (fake_gate.closed) return XIAOTAI_ERR_PLATFORM_GATED;
    fake_gate.closed = true;
    fake_gate.ticket = 77;
    *ticket = fake_gate.ticket;
    return XIAOTAI_OK;
}

bool xiaotai_platform_gate_drained(uint32_t ticket)
{
    return fake_gate.closed && ticket == fake_gate.ticket && fake_gate.drained;
}

xiaotai_err_t xiaotai_platform_gate_open(uint32_t ticket)
{
    if (!fake_gate.closed || ticket != fake_gate.ticket) return XIAOTAI_ERR_INVALID_ARG;
    ++fake_gate.opens;
    fake_gate.closed = false;
    return XIAOTAI_OK;
}
