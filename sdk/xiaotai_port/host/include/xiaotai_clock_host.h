// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The fixture may change inputs with clock waiters active; reset requires quiescence. */
void xiaotai_clock_host_reset(void);
void xiaotai_clock_host_set_wall_ms(int64_t unix_ms);
void xiaotai_clock_host_advance_ms(int64_t delta);
void xiaotai_clock_host_answer(int64_t wall_ms, uint32_t delay_ms);
void xiaotai_clock_host_hold(bool hold);
void xiaotai_clock_host_timeout_next(unsigned count);
void xiaotai_clock_host_fail_starts(unsigned count);
void xiaotai_clock_host_probe_result(bool accepted, int64_t wall_ms, uint32_t invalid);
unsigned xiaotai_clock_host_probes(void);

#ifdef __cplusplus
}
#endif
