// SPDX-License-Identifier: MIT
/* A recording through flash scratch: written behind the recording a second
 * at a time, longer than its ring, read back whole and unchanged, never
 * written where flash was not erased; and without scratch, refused before
 * anything starts. */
#include "recognition_spill.h"
#include "xiaotai/framework.h"
#include "xiaotai/media.h"
#include "xiaotai/port/os.h"
#include "xiaotai_scratch_host.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(1); } } while (0)

/* What the spill needs of the media engine and the framework. */
void xiaotai_media_record_taken(uint32_t id, uint32_t samples)
{
    (void)id;
    (void)samples;
}

xiaotai_err_t xiaotai_run(void (*fn)(void *ctx), void *ctx)
{
    (void)fn;
    (void)ctx;
    return XIAOTAI_ERR_INVALID_STATE;     /* no owner here: the spill keeps trying, harmlessly */
}

static void flushed(void *ctx) { (void)ctx; }
static int16_t sample(uint32_t n) { return (int16_t)(n * 7919U + 13U); }

/* True once the spill holds the first `samples` samples. */
static bool written(rc_spill_t *spill, uint32_t samples)
{
    int16_t last;
    for (unsigned i = 0; i < 4000U; ++i) {
        if (rc_spill_read(spill, ((size_t)samples - 1U) * sizeof(int16_t), &last, sizeof(last)) == XIAOTAI_OK)
            return last == sample(samples - 1U);
        xiaotai_delay_ms(1);
    }
    return false;
}

int main(void)
{
    rc_spill_t *spill = NULL;
    int16_t *ring = NULL;
    uint32_t ring_samples = 0;
    const uint32_t rate = 16000U, total = rate * 10U + 123U;   /* more than the ring, not whole seconds */
    CHECK(!rc_spill_possible((size_t)total * 2U, rate));
    CHECK(rc_spill_start(&spill, total, rate, &ring, &ring_samples, flushed, NULL) == XIAOTAI_ERR_NOT_SUPPORTED && !spill);

    xiaotai_scratch_host_set_size(512U * 1024U);
    CHECK(rc_spill_possible((size_t)total * 2U, rate) && !rc_spill_possible(600U * 1024U, rate));
    CHECK(rc_spill_start(&spill, total, rate, &ring, &ring_samples, flushed, NULL) == XIAOTAI_OK && spill);
    CHECK(ring && ring_samples == RC_SPILL_RING_SECONDS * rate);
    CHECK(!rc_spill_flushed(spill));
    for (uint32_t made = 0; made < total;) {
        const uint32_t step = rate < total - made ? rate : total - made;
        for (uint32_t k = 0; k < step; ++k) ring[(made + k) % ring_samples] = sample(made + k);
        made += step;
        if (made < total) rc_spill_feed(spill, made);
        else rc_spill_finish(spill, made);
        CHECK(written(spill, made));
    }
    for (unsigned i = 0; i < 2000U && !rc_spill_flushed(spill); ++i) xiaotai_delay_ms(1);
    CHECK(rc_spill_flushed(spill) && rc_spill_error(spill) == XIAOTAI_OK);

    /* Read back in the upload's pieces, across the ring's turns. */
    static int16_t back[1000];
    for (uint32_t at = 0; at < total;) {
        const uint32_t count = total - at < 1000U ? total - at : 1000U;
        CHECK(rc_spill_read(spill, (size_t)at * 2U, back, (size_t)count * 2U) == XIAOTAI_OK);
        for (uint32_t k = 0; k < count; ++k) CHECK(back[k] == sample(at + k));
        at += count;
    }
    CHECK(rc_spill_read(spill, (size_t)total * 2U - 1U, back, 2) == XIAOTAI_ERR_INVALID_STATE);   /* past the end */
    xiaotai_scratch_host_stats_t stats;
    xiaotai_scratch_host_stats(&stats);
    CHECK(stats.erases >= 1U && stats.writes >= total / 2048U && !stats.unerased_writes);
    rc_spill_stop(spill);
    xiaotai_delay_ms(400);       /* its task ends and frees it */
    printf("spill ok: %u erases, %u writes, %u reads\n", (unsigned)stats.erases, (unsigned)stats.writes, (unsigned)stats.reads);
    return 0;
}
