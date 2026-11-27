// SPDX-License-Identifier: MIT
#include "clock_internal.h"
#include <string.h>

#define NTP_EPOCH_DELTA 2208988800ULL
#define NTP_ERA (1ULL << 32)
#define MAX_UNIX_SECONDS 4102444800ULL

static uint64_t read_word(const uint8_t *bytes, size_t count)
{
    uint64_t value = 0;
    for (size_t i = 0; i < count; ++i) value = (value << 8) | bytes[i];
    return value;
}

void clock_ntp_request(uint8_t out[CLOCK_NTP_BYTES], uint64_t cookie)
{
    memset(out, 0, CLOCK_NTP_BYTES);
    out[0] = (4U << 3) | 3U;
    for (size_t i = CLOCK_NTP_BYTES; i > 40; cookie >>= 8) out[--i] = (uint8_t)cookie;
}

bool clock_ntp_answer(const uint8_t *packet, size_t len, uint64_t cookie,
                      uint64_t elapsed_us, uint64_t *unix_us)
{
    if (!packet || len < CLOCK_NTP_BYTES || !unix_us || elapsed_us > 3000000 ||
        (packet[0] >> 6) == 3U || (packet[0] & 7U) != 4U ||
        packet[1] < 1U || packet[1] > 15U || read_word(packet + 24, 8) != cookie) return false;
    const unsigned version = (packet[0] >> 3) & 7U;
    if (version != 3U && version != 4U) return false;
    uint64_t seconds = read_word(packet + 40, 4);
    if (seconds < NTP_EPOCH_DELTA) seconds += NTP_ERA;
    seconds -= NTP_EPOCH_DELTA;
    if (seconds <= XIAOTAI_CLOCK_VALID_AFTER_S || seconds >= MAX_UNIX_SECONDS) return false;
    const uint64_t fraction = read_word(packet + 44, 4);
    *unix_us = seconds * 1000000U + ((fraction * 1000000U) >> 32) + elapsed_us / 2U;
    return true;
}

bool clock_ntp_mean(const uint64_t *samples, size_t count, uint64_t *unix_us)
{
    if (!samples || !count || count > XIAOTAI_CLOCK_SERVERS_MAX || !unix_us) return false;
    uint64_t min = UINT64_MAX, max = 0;
    for (size_t i = 0; i < count; ++i) {
        if (samples[i] < min) min = samples[i];
        if (samples[i] > max) max = samples[i];
    }
    if (max - min > 2000000U) return false;
    uint64_t offset = 0;
    for (size_t i = 0; i < count; ++i) offset += samples[i] - min;
    *unix_us = min + offset / count;
    return true;
}
