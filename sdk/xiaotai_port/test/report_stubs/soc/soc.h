// SPDX-License-Identifier: MIT
#pragma once
#include <stdint.h>

uint32_t report_test_reg_read(unsigned reg);
#define REG_READ(reg) report_test_reg_read(reg)
