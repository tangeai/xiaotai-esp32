// SPDX-License-Identifier: MIT
#pragma once
/* Test controls of the host scratch (xiaotai/port/scratch.h). */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t erases, writes, reads;
    uint32_t unerased_writes;       /* a write that needed an erase first: refused, as flash would garble it */
} xiaotai_scratch_host_stats_t;

/* Gives the scratch `bytes` of fresh, unerased memory; 0: none (the default). */
void xiaotai_scratch_host_set_size(size_t bytes);
void xiaotai_scratch_host_stats(xiaotai_scratch_host_stats_t *stats);

#ifdef __cplusplus
}
#endif
