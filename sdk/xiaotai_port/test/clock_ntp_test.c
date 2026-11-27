// SPDX-License-Identifier: MIT
#include "clock_internal.h"
#include <assert.h>
#include <string.h>

#define EPOCH_DIFF 2208988800ULL
#define SENT UINT64_C(0xe65a3b0101020304)

static void reply(uint8_t *bytes, uint8_t v, uint8_t stratum, uint64_t epoch_s)
{
    uint8_t request[CLOCK_NTP_BYTES];
    clock_ntp_request(request, SENT);
    assert(request[0] == 0x23);
    for (size_t i = 1; i < 40; ++i) assert(!request[i]);
    memset(bytes, 0, CLOCK_NTP_BYTES);
    bytes[0] = v;
    bytes[1] = stratum;
    memcpy(bytes + 24, request + 40, 8);
    const uint32_t ntp = (uint32_t)(epoch_s + EPOCH_DIFF);
    for (size_t i = 0; i < 4; ++i) bytes[40 + i] = (uint8_t)(ntp >> (24 - 8 * i));
    bytes[44] = 0x80; /* Bare half second, independent of the round-trip estimate. */
}

int main(void)
{
    uint8_t bytes[CLOCK_NTP_BYTES];
    uint64_t converted = 0;
    reply(bytes, 0x24, 1, 1800000000);
    assert(clock_ntp_answer(bytes, sizeof(bytes), SENT, 310, &converted));
    assert(converted == UINT64_C(1800000000500155));
    assert(!clock_ntp_answer(bytes, sizeof(bytes) - 1, SENT, 310, &converted));
    assert(!clock_ntp_answer(bytes, sizeof(bytes), SENT ^ 1, 310, &converted));
    assert(!clock_ntp_answer(bytes, sizeof(bytes), SENT, 3000001, &converted));
    static const uint8_t bad_header[] = {0xe4, 0x14, 0x2c, 0x23, 0x25}; /* LI, version, mode. */
    for (size_t i = 0; i < sizeof(bad_header); ++i) {
        bytes[0] = bad_header[i];
        assert(!clock_ntp_answer(bytes, sizeof(bytes), SENT, 310, &converted));
    }
    reply(bytes, 0x1c, 15, 2200000000ULL); /* Version 3, second NTP era. */
    assert(clock_ntp_answer(bytes, sizeof(bytes), SENT, 0, &converted));
    assert(converted == UINT64_C(2200000000500000));
    bytes[1] = 0;
    assert(!clock_ntp_answer(bytes, sizeof(bytes), SENT, 0, &converted));
    bytes[1] = 16;
    assert(!clock_ntp_answer(bytes, sizeof(bytes), SENT, 0, &converted));
    reply(bytes, 0x24, 1, 1700000000);
    assert(!clock_ntp_answer(bytes, sizeof(bytes), SENT, 310, &converted));
    reply(bytes, 0x24, 1, 4102444800ULL);
    assert(!clock_ntp_answer(bytes, sizeof(bytes), SENT, 310, &converted));

    const uint64_t in = UINT64_C(1800000000000000);
    uint64_t peers[] = {in + 2000000, in, in + 1000000};
    assert(!clock_ntp_mean(peers, 0, &converted));
    assert(clock_ntp_mean(peers, 1, &converted) && converted == in + 2000000);
    assert(clock_ntp_mean(peers, 2, &converted) && converted == in + 1000000);
    assert(clock_ntp_mean(peers, 3, &converted) && converted == in + 1000000);
    peers[0]++;
    assert(!clock_ntp_mean(peers, 2, &converted));
    return 0;
}
