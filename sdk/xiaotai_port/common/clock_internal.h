// SPDX-License-Identifier: MIT
#pragma once
#include "xiaotai/port/clock.h"

/* Names are owned by the clock and immutable between init and host-only reset. */
typedef struct {
    char servers[XIAOTAI_CLOCK_SERVERS_MAX][XIAOTAI_CLOCK_NAME_MAX];
    size_t count;
    bool keep_running;
} clock_servers_t;

int64_t clock_wall_ms(void);
bool clock_wall_set(int64_t unix_ms);
uint32_t clock_service_budget(size_t servers);
/* Enqueue without waiting on the network. Completion must not run inline. */
bool clock_service_start(const clock_servers_t *servers, bool release_after_start);
/* Host simulates background responses; the firmware backend dispatches them. */
void clock_service_poll(void);
void clock_service_wait_timed_out(void);
bool clock_parallel_probe(const clock_servers_t *servers, int64_t *unix_ms, uint32_t *invalid);
void clock_service_started(bool ok);
bool clock_service_answer(int64_t before_ms, int64_t after_ms);
void clock_service_stopped(void);

#define CLOCK_NTP_BYTES 48U
void clock_ntp_request(uint8_t out[CLOCK_NTP_BYTES], uint64_t cookie);
bool clock_ntp_answer(const uint8_t *packet, size_t len, uint64_t cookie,
                      uint64_t elapsed_us, uint64_t *unix_us);
bool clock_ntp_mean(const uint64_t *samples, size_t count, uint64_t *unix_us);

void clock_fixture_reset(void);
