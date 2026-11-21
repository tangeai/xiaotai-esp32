// SPDX-License-Identifier: MIT
/* Fixed-frame delay line (src/effects/frame_stream.h): output is the
 * processed stream exactly one frame late with a silent first frame, for any
 * block sizes and in place; the step sees whole frames in order; a failed
 * frame clears the line and is reported; reset restarts the silent frame. */
#include <stdio.h>
#include <string.h>
#include "effects/frame_stream.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

#define FRAME 160U
#define TOTAL 4000U

typedef struct {
    unsigned calls;
    unsigned fail_at;   /* 1-based call that fails; 0: never */
    int16_t next;       /* first sample the next frame must start with */
    bool ordered;
} step_t;

/* Doubles the frame, after checking it is the next whole frame of the input ramp. */
static bool step_double(void *ctx, int16_t *input, int16_t *output, size_t frame)
{
    step_t *step = ctx;
    ++step->calls;
    if (frame != FRAME || input[0] != step->next) step->ordered = false;
    for (size_t i = 1; i < frame; ++i) {
        if (input[i] != (int16_t)(input[i - 1] + 1)) step->ordered = false;
    }
    step->next = (int16_t)(input[frame - 1] + 1);
    for (size_t i = 0; i < frame; ++i) output[i] = (int16_t)(2 * input[i]);
    input[0] = -1; /* scratch: the line must not depend on it */
    return step->fail_at == 0 || step->calls != step->fail_at;
}

static void ramp(int16_t *pcm, size_t count)
{
    for (size_t i = 0; i < count; ++i) pcm[i] = (int16_t)(i % 10000);
}

/* Runs TOTAL samples through a fresh line in blocks cycling through sizes. */
static int run(const size_t *sizes, size_t kinds, bool in_place, int16_t *out, step_t *step)
{
    static int16_t storage[2 * FRAME], input[TOTAL];
    ramp(input, TOTAL);
    memset(storage, 0x55, sizeof(storage));
    xiaotai_media_frame_stream_t line;
    xiaotai_media_frame_stream_init(&line, storage, FRAME);
    *step = (step_t){.ordered = true};
    if (in_place) memcpy(out, input, sizeof(input));
    size_t offset = 0;
    for (size_t k = 0; offset < TOTAL; ++k) {
        size_t n = sizes[k % kinds];
        if (n > TOTAL - offset) n = TOTAL - offset;
        const int16_t *source = in_place ? out + offset : input + offset;
        CHECK(xiaotai_media_frame_stream_process(&line, source, out + offset, n, step_double, step));
        offset += n;
    }
    return 0;
}

int main(void)
{
    static int16_t whole[TOTAL], pieces[TOTAL], inplace[TOTAL];
    step_t step;
    const size_t all[] = {TOTAL}, odd[] = {1, 7, 320, 513, 33};
    CHECK(run(all, 1, false, whole, &step) == 0);
    CHECK(step.ordered && step.calls == TOTAL / FRAME);
    /* One frame of silence (despite the storage's previous content), then the
     * processed input, exactly one frame late. */
    for (size_t i = 0; i < FRAME; ++i) CHECK(whole[i] == 0);
    for (size_t i = FRAME; i < TOTAL; ++i) CHECK(whole[i] == (int16_t)(2 * ((i - FRAME) % 10000)));
    CHECK(run(odd, 5, false, pieces, &step) == 0);
    CHECK(step.ordered && step.calls == TOTAL / FRAME && memcmp(whole, pieces, sizeof(whole)) == 0);
    CHECK(run(odd, 5, true, inplace, &step) == 0);
    CHECK(step.ordered && memcmp(whole, inplace, sizeof(whole)) == 0);

    /* A failed third frame: reported, the line cleared, the next frame out silent. */
    static int16_t storage[2 * FRAME], input[TOTAL], out[TOTAL];
    ramp(input, TOTAL);
    xiaotai_media_frame_stream_t line;
    xiaotai_media_frame_stream_init(&line, storage, FRAME);
    step = (step_t){.fail_at = 3, .ordered = true};
    CHECK(xiaotai_media_frame_stream_process(&line, input, out, 2 * FRAME, step_double, &step));
    CHECK(!xiaotai_media_frame_stream_process(&line, input + 2 * FRAME, out + 2 * FRAME, 100 + FRAME, step_double,
                                              &step));
    CHECK(step.calls == 3 && line.position == 0);
    for (size_t i = 0; i < 2 * FRAME; ++i) CHECK(storage[i] == 0);
    /* Written before the failure: the processed second frame. */
    for (size_t i = 2 * FRAME; i < 3 * FRAME; ++i) CHECK(out[i] == (int16_t)(2 * (i - FRAME)));
    /* The caller drops the block and goes on: silence for one frame, then the stream again. */
    step.next = (int16_t)(3 * FRAME);
    CHECK(xiaotai_media_frame_stream_process(&line, input + 3 * FRAME, out, 2 * FRAME, step_double, &step));
    for (size_t i = 0; i < FRAME; ++i) CHECK(out[i] == 0);
    for (size_t i = FRAME; i < 2 * FRAME; ++i) CHECK(out[i] == (int16_t)(2 * (i + 2 * FRAME)));
    CHECK(step.ordered);

    /* Reset (the effect's reset): a frame of silence again, mid-frame state dropped. */
    CHECK(xiaotai_media_frame_stream_process(&line, input, out, 17, step_double, &step));
    xiaotai_media_frame_stream_reset(&line);
    CHECK(line.position == 0);
    step.next = 0;
    CHECK(xiaotai_media_frame_stream_process(&line, input, out, 2 * FRAME, step_double, &step));
    for (size_t i = 0; i < FRAME; ++i) CHECK(out[i] == 0);
    for (size_t i = FRAME; i < 2 * FRAME; ++i) CHECK(out[i] == (int16_t)(2 * (i - FRAME)));
    CHECK(step.ordered);
    printf("PASS: media_frame_stream\n");
    return 0;
}
