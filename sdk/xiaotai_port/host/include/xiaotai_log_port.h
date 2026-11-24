// SPDX-License-Identifier: MIT
#pragma once
/* xiaotai/port/log.h on the host: stderr, behind the same gate as on a device;
 * XIAOTAI_HOST_LOG_QUIET silences info/debug whatever the threshold. */
#include <stdio.h>
#include <stdlib.h>

#define XIAOTAI_LOG_PORT(level, letter, tag, format, ...)                                \
    do {                                                                                 \
        if (((level) <= XIAOTAI_LOG_LEVEL_WARN || !getenv("XIAOTAI_HOST_LOG_QUIET")) &&  \
            xiaotai_log_enabled((level), (tag))) {                                       \
            fprintf(stderr, letter " (%s) " format "\n", (tag), ##__VA_ARGS__);          \
        }                                                                                \
    } while (0)
