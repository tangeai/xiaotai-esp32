// SPDX-License-Identifier: MIT
// The touch unit on traces with the faults of real panels: jitter, spikes,
// lost samples, uneven periods. Distances are in units of 1/240 of the shorter
// side, and every trace runs on a small and on a large display.
#include "ui_touch.h"
#include "ui_test.h"

#include <math.h>
#include <string.h>

enum { PERIOD_US = 10000 };

static uint32_t seed;

/* Deterministic noise, -1 .. 1. */
static float noise(void)
{
    seed = seed * 1664525U + 1013904223U;
    return (float)(int32_t)seed / 2147483648.0f;
}

typedef struct {
    ui_touch_t touch;
    int64_t at;
    float unit;
    ui_contact_t last;
    unsigned contacts, presses, releases;
} run_t;

static void begin(run_t *r, uint16_t width, uint16_t height)
{
    *r = (run_t){.at = 1000000};
    seed = 7U;
    ui_touch_init(&r->touch, width, height);
    r->unit = r->touch.unit;
}

static void sample(run_t *r, float x, float y, bool down)
{
    const ui_sample_t s = {.at_us = r->at, .x = (int32_t)lroundf(x * r->unit), .y = (int32_t)lroundf(y * r->unit),
                           .down = down};
    ui_contact_t out[2];
    const size_t n = ui_touch_feed(&r->touch, &s, out);
    for (size_t i = 0; i < n; ++i) {
        if (out[i].down && !r->last.down) ++r->presses;
        if (!out[i].down && r->last.down) ++r->releases;
        r->last = out[i];
        ++r->contacts;
    }
    r->at += PERIOD_US;
}

/* A tap that trembles: pressed where it landed, at once, and released with the
 * first empty sample, where it was. */
static int tap(uint16_t w, uint16_t h)
{
    run_t r;
    begin(&r, w, h);
    sample(&r, 100, 100, true);
    CHECK(r.contacts == 1 && r.presses == 1);
    CHECK(r.last.x == (int32_t)lroundf(100 * r.unit) && r.last.y == (int32_t)lroundf(100 * r.unit));
    for (int i = 0; i < 6; ++i) {
        sample(&r, 100 + 2 * noise(), 100 + 2 * noise(), true);
        CHECK(fabsf((float)r.last.x - 100 * r.unit) <= 2 * r.unit + 1 && fabsf((float)r.last.y - 100 * r.unit) <= 2 * r.unit + 1);
    }
    sample(&r, 0, 0, false);
    CHECK(r.releases == 1 && !r.last.down && r.last.vx == 0 && r.last.vy == 0);
    CHECK(fabsf((float)r.last.x - 100 * r.unit) <= 2 * r.unit + 1);
    sample(&r, 0, 0, false);
    CHECK(r.releases == 1 && r.presses == 1);
    /* One sample is a contact too: a short tap is not lost. */
    sample(&r, 50, 60, true);
    sample(&r, 0, 0, false);
    CHECK(r.presses == 2 && r.releases == 2 && r.last.x == (int32_t)lroundf(50 * r.unit));
    return 0;
}

/* A finger at rest: what is left of two units of jitter. */
static int rest(uint16_t w, uint16_t h)
{
    run_t r;
    begin(&r, w, h);
    float raw = 0, seen = 0;
    sample(&r, 100, 100, true);
    for (int i = 0; i < 200; ++i) {
        const float x = 100 + 2 * noise();
        sample(&r, x, 100, true);
        if (i < 20) continue;
        raw += (x - 100) * (x - 100);
        const float e = (float)r.last.x / r.unit - 100;
        seen += e * e;
    }
    printf("rest %ux%u: jitter %.2f -> %.2f units\n", w, h, sqrtf(raw / 180), sqrtf(seen / 180));
    CHECK(sqrtf(seen) < 0.65f * sqrtf(raw));
    return 0;
}

/* A swipe with jitter: how far behind the finger it is, how far past its end
 * it goes, and the speed it leaves with. */
static int swipe(uint16_t w, uint16_t h, float speed)
{
    run_t r;
    begin(&r, w, h);
    const float step = speed * PERIOD_US / 1000000.0f;
    float at = 20, late = 0, worst = 0;
    int n = 0;
    sample(&r, at, 100, true);
    for (int i = 0; i < 14; ++i) {
        at += step;
        sample(&r, at + noise(), 100 + noise(), true);
        if (i < 4) continue;
        const float behind = at - (float)r.last.x / r.unit;
        late += behind;
        if (behind > worst) worst = behind;
        ++n;
    }
    const float lag_ms = late / (float)n / speed * 1000.0f;
    sample(&r, 0, 0, false);
    CHECK(r.releases == 0);   /* one empty sample in a drag may be a lost one */
    sample(&r, 0, 0, false);
    sample(&r, 0, 0, false);
    CHECK(r.releases == 1 && r.presses == 1);
    const float left = r.last.vx / r.unit;
    printf("swipe %ux%u %4.0f u/s: lag %.1f ms (worst %.1f units), leaves at %.0f u/s\n", w, h, speed, lag_ms, worst, left);
    CHECK(fabsf(lag_ms) < (speed < 200 ? 22.0f : 13.0f) && fabsf(left - speed) < 0.12f * speed + 12.0f);
    return 0;
}

/* A finger that stops dead: nothing runs on past it for long. */
static int stop(uint16_t w, uint16_t h)
{
    run_t r;
    begin(&r, w, h);
    float at = 20, over = 0;
    sample(&r, at, 100, true);
    for (int i = 0; i < 14; ++i) sample(&r, at += 8, 100, true);
    for (int i = 0; i < 20; ++i) {
        sample(&r, at + 0.5f * noise(), 100, true);
        const float past = (float)r.last.x / r.unit - at;
        if (past > over) over = past;
    }
    printf("stop %ux%u: %.1f units past the end, %.1f after 200 ms\n", w, h, over, (float)r.last.x / r.unit - at);
    CHECK(over < 6.0f && fabsf((float)r.last.x / r.unit - at) < 1.0f);
    for (int i = 0; i < 3; ++i) sample(&r, 0, 0, false);
    CHECK(r.releases == 1 && fabsf(r.last.vx) < 30 * r.unit);   /* it rested before it left */
    return 0;
}

/* Panel faults: a spike, a lost sample, another finger, a failed read. */
static int faults(uint16_t w, uint16_t h)
{
    run_t r;
    begin(&r, w, h);
    sample(&r, 60, 60, true);
    for (int i = 1; i < 6; ++i) sample(&r, 60 + 4 * (float)i, 60, true);
    const unsigned before = r.contacts;
    sample(&r, 200, 10, true);                /* a spike */
    CHECK(r.contacts == before);
    sample(&r, 88, 60, true);
    CHECK(r.presses == 1 && r.releases == 0 && fabsf((float)r.last.x / r.unit - 86) < 6);
    sample(&r, 0, 0, false);                  /* a lost sample */
    sample(&r, 96, 60, true);
    CHECK(r.presses == 1 && r.releases == 0);
    sample(&r, 220, 160, true);               /* another finger took over */
    sample(&r, 221, 161, true);
    CHECK(r.presses == 2 && r.releases == 1 && r.last.down);
    CHECK(fabsf((float)r.last.x / r.unit - 221) < 3 && fabsf((float)r.last.y / r.unit - 161) < 3);
    const ui_sample_t bad = {.at_us = r.at, .failed = true};
    ui_contact_t out[2];
    CHECK(ui_touch_feed(&r.touch, &bad, out) == 1 && !out[0].down);
    CHECK(ui_touch_feed(&r.touch, &bad, out) == 0);
    /* Off the edge it stays on the display. */
    begin(&r, w, h);
    sample(&r, 1, 1, true);
    for (int i = 0; i < 4; ++i) sample(&r, 0, 0, true);
    CHECK(r.last.x >= 0 && r.last.y >= 0);
    return 0;
}

int main(void)
{
    static const uint16_t sizes[][2] = {{320, 240}, {480, 320}, {1024, 600}};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        const uint16_t w = sizes[i][0], h = sizes[i][1];
        if (tap(w, h) || rest(w, h) || swipe(w, h, 150) || swipe(w, h, 400) || swipe(w, h, 900) || stop(w, h) ||
            faults(w, h)) return 1;
    }
    return 0;
}
