// SPDX-License-Identifier: MIT
/* xiaotai/port/sys.h on the host. */
#include "xiaotai/port/sys.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "xiaotai/port/log.h"

/* The host log keeps its own filter (XIAOTAI_HOST_LOG_QUIET); the threshold
 * is only remembered, so console code reads back what it set. */
static atomic_uint s_log_level = 3U;

xiaotai_err_t xiaotai_log_set_level(uint8_t level)
{
    if (level > XIAOTAI_LOG_LEVEL_DEBUG) return XIAOTAI_ERR_INVALID_ARG;
    atomic_store(&s_log_level, level);
    return XIAOTAI_OK;
}

uint8_t xiaotai_log_level(void) { return (uint8_t)atomic_load(&s_log_level); }
uint8_t xiaotai_log_level_max(void) { return XIAOTAI_LOG_LEVEL_DEBUG; }

void xiaotai_firmware(xiaotai_firmware_t *info)
{
    if (!info) return;
    long cores = sysconf(_SC_NPROCESSORS_ONLN);
    *info = (xiaotai_firmware_t){
        .version = "host",
        .platform = "host",
        .chip = "host",
        .cores = cores > 0 ? (unsigned)cores : 1U,
    };
}

uint32_t xiaotai_random(void)
{
    uint32_t value = 0;
    FILE *source = fopen("/dev/urandom", "rb");
    if (!source || fread(&value, sizeof(value), 1, source) != 1) value = (uint32_t)rand();
    if (source) fclose(source);
    return value;
}

void xiaotai_restart(void)
{
    exit(EXIT_FAILURE);
}
