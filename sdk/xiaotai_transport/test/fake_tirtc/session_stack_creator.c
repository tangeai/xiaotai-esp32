// SPDX-License-Identifier: MIT
#include <stdint.h>

extern void *fixture_rtc_thread_create(uint32_t bytes);
void *rtc_thread_create(uint32_t bytes);

void *rtc_thread_create(uint32_t bytes) { return fixture_rtc_thread_create(bytes); }
