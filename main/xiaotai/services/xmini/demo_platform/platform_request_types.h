// SPDX-License-Identifier: MIT
#pragma once
#include "demo_platform.h"

typedef struct {
    platform_operation_t operation;
    uint32_t generation;
    unsigned epoch;
    char body[768];
} request_t;
