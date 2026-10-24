// SPDX-License-Identifier: MIT
#include <assert.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ACTUAL_HEAP_CAPABILITY_MACROS */

static unsigned calls[3][3], mode;
static const unsigned masks[] = {0x804, 0x80c, 0x8000};
static size_t query(uint32_t caps, unsigned kind)
{
    unsigned index = 0;
    while (index < 3 && caps != masks[index]) ++index;
    assert(index < 3 && kind < 3);
    ++calls[index][kind];
    if (mode == 1) return 0;
    if (mode == 2) return UINT32_MAX;
    return (index + 1) * 1000 + (kind == 0 ? 100 : kind == 1 ? 70 : 30);
}
static size_t heap_caps_get_free_size(uint32_t caps) { return query(caps, 0); }
static size_t heap_caps_get_largest_free_block(uint32_t caps) { return query(caps, 1); }
static size_t heap_caps_get_minimum_free_size(uint32_t caps) { return query(caps, 2); }

int main(void)
{
    _Static_assert(UINT_MAX == UINT32_MAX, "Match C3 unsigned output width");
    _Static_assert((MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) == 0x804, "Internal mask");
    _Static_assert((MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT | MALLOC_CAP_DMA) == 0x80c, "DMA mask");
    _Static_assert(MALLOC_CAP_RTCRAM == 0x8000, "Actual IDF RTC fast memory mask");
    for (mode = 0; mode < 3; ++mode) {
        memset(calls, 0, sizeof(calls));
        /* ACTUAL_HEAPCAP_STATUS */
        for (unsigned pool = 0; pool < 3; ++pool)
            for (unsigned kind = 0; kind < 3; ++kind) assert(calls[pool][kind] == 1);
    }
    return 0;
}
