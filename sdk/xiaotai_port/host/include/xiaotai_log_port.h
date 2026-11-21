// SPDX-License-Identifier: MIT
#pragma once
/* xiaotai/port/log.h on the host: stderr; XIAOTAI_HOST_LOG_QUIET silences info/debug. */
#include <stdio.h>
#include <stdlib.h>

#define XIAOTAI_LOG_PORT(level, tag, ...)                                                   \
    do {                                                                                    \
        if ((level) == 'E' || (level) == 'W' || !getenv("XIAOTAI_HOST_LOG_QUIET")) {        \
            fprintf(stderr, "%c (%s) ", (level), (tag));                                    \
            fprintf(stderr, __VA_ARGS__);                                                   \
            fputc('\n', stderr);                                                            \
        }                                                                                   \
    } while (0)
