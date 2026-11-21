// SPDX-License-Identifier: MIT
/* xiaotai/port/sys.h on the host. */
#include "xiaotai/port/sys.h"

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

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
