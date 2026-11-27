// SPDX-License-Identifier: MIT
/* xiaotai/port/sys.h on the host. */
#include "xiaotai/port/sys.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include "xiaotai/port/log.h"

/* The host has no platform log: the system threshold is only remembered, so
 * console code reads back what it set. */
static atomic_uint s_system_level = XIAOTAI_LOG_LEVEL_WARN;

xiaotai_err_t xiaotai_log_set_system_level(uint8_t level)
{
    if (level > XIAOTAI_LOG_LEVEL_INFO) return XIAOTAI_ERR_INVALID_ARG;
    atomic_store(&s_system_level, level);
    return XIAOTAI_OK;
}

uint8_t xiaotai_log_system_level(void) { return (uint8_t)atomic_load(&s_system_level); }
uint8_t xiaotai_log_system_level_max(void) { return XIAOTAI_LOG_LEVEL_INFO; }
void xiaotai_log_start(void) {}
void xiaotai_log_quiet_for_restart(void) {}

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
