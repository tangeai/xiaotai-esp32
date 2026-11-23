// SPDX-License-Identifier: MIT
/* The engine's task loop (src/wake.c) against a scripted model, a fake media
 * tap and a fake owner queue, on a virtual clock with absolute schedules:
 * 512-sample frames every 32 ms (16 kHz) from the moment the model is ready,
 * engine passes 20 ms after the previous one ended, as the task runs them.
 *
 * Covered: registration and descriptor checks, the tap (name, flags), listen
 * and its refusals, the load retry schedule (5, 10, 20, 30, 30 s) with every
 * error kind and the self-test, memory taken and given back, arming, scoring
 * cadence and window content, every counter, detections in subscription order
 * on the owner task, cooldown, the threshold, the four rejections, gaps and
 * generations, wrong rates, delivery while the owner queue is full and the
 * 1000 ms limit, the posted-detection ring, and the real task on the host
 * port. */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "fakes.h"
#include "internal.h"
#include "wake_queue.h"
#include "xiaotai/port/os.h"
#include "xiaotai_mem_host.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

#define T0 1000000LL          /* virtual time when the model becomes ready */
#define FRAME 512U
#define FRAME_US 32000LL
#define WINDOW 16192U

/* ------------------------------------------------------------ the schedule */

static bool s_audio;          /* frames flow on the schedule */
static int64_t s_origin_us;   /* frame k ends at s_origin_us + FRAME_US * (k + 1) */
static uint64_t s_next_frame; /* index of the next scheduled frame */
static int64_t s_next_step_us;
static int16_t s_pcm[FRAME + 1];

static int16_t sample(uint64_t n) { return (int16_t)(n % 30000U); }

static void send_frame(uint64_t k)
{
    for (unsigned i = 0; i < FRAME; ++i) s_pcm[i] = sample(k * FRAME + i);
    fake_media_pcm(s_pcm, FRAME, 16000, s_origin_us + FRAME_US * (int64_t)(k + 1));
}

/* Runs frames and engine passes in time order up to and including until_us.
 * A pass that scored for a while leaves the clock later; frames that were due
 * meanwhile arrive then (the capture task kept running), the next pass comes
 * 20 ms after it ended. */
static void run_until(int64_t until_us)
{
    for (;;) {
        int64_t frame_at = s_audio ? s_origin_us + FRAME_US * (int64_t)(s_next_frame + 1) : INT64_MAX;
        int64_t at = frame_at <= s_next_step_us ? frame_at : s_next_step_us;
        if (at > until_us) break;
        if (g_fake_now_us < at) g_fake_now_us = at;
        if (frame_at <= s_next_step_us) {
            send_frame(s_next_frame++);
        } else {
            uint32_t wait_ms = wake_engine_step();
            s_next_step_us = g_fake_now_us + (int64_t)wait_ms * 1000;
        }
    }
    if (g_fake_now_us < until_us) g_fake_now_us = until_us;
}

/* First frame of the schedule whose end is at or after at_us. */
static uint64_t first_frame_from(int64_t at_us)
{
    int64_t k = (at_us - s_origin_us + FRAME_US - 1) / FRAME_US - 1;
    return k < 0 ? 0U : (uint64_t)k;
}

/* Capture time of the end of a window ending end samples into a run that
 * began with frame k0: the newest frame's end less the samples after the
 * grid position, as the engine computes it. */
static int64_t window_end_us(uint64_t k0, uint64_t end)
{
    uint64_t total = (end + FRAME - 1U) / FRAME * FRAME;
    int64_t newest = s_origin_us + FRAME_US * (int64_t)(k0 + total / FRAME);
    return newest - (int64_t)((total - end) * 1000000U / 16000U);
}

/* -------------------------------------------------------------- subscribers */

typedef struct {
    char who;
    xiaotai_wake_detection_t detection;
} seen_t;

static seen_t s_seen[64];
static size_t s_seen_count;
static int s_ctx_b;

static void subscriber_a(const xiaotai_wake_detection_t *detection, void *ctx)
{
    if (ctx == NULL && s_seen_count < 64) s_seen[s_seen_count++] = (seen_t){'a', *detection};
}

static void subscriber_b(const xiaotai_wake_detection_t *detection, void *ctx)
{
    if (ctx == &s_ctx_b && s_seen_count < 64) s_seen[s_seen_count++] = (seen_t){'b', *detection};
}

/* ------------------------------------------------------------------- set-up */

static void setup(const xiaotai_media_param_t *params, size_t count)
{
    xiaotai_wake_reset();
    fake_media_reset();
    fake_owner_reset();
    scripted_model_reset();
    xiaotai_mem_host_clear();
    fake_media_set_params(params, count);
    g_fake_now_us = T0;
    wake_engine_set_clock(fake_now);
    s_audio = false;
    s_origin_us = T0;
    s_next_frame = 0;
    s_next_step_us = T0;
    s_seen_count = 0;
}

static void script(size_t index, script_step_t step)
{
    g_script.steps[index] = step;
    if (g_script.step_count <= index) g_script.step_count = index + 1U;
}

/* Model registered, two subscribers, init, listening wanted and the gate open
 * before the model is ready, one pass at T0 loads it; frames flow from T0. */
static int make_ready(bool audio)
{
    CHECK(xiaotai_wake_model_register(&g_script.model) == XIAOTAI_OK);
    CHECK(xiaotai_wake_subscribe(subscriber_a, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_wake_subscribe(subscriber_b, &s_ctx_b) == XIAOTAI_OK);
    CHECK(xiaotai_engine_wake_engine_init() == XIAOTAI_OK);
    CHECK(xiaotai_wake_listen(true) == XIAOTAI_OK);
    fake_media_gate(true);
    g_fake_now_us = T0;
    CHECK(wake_engine_step() == WAKE_POLL_MS);
    xiaotai_wake_status_t status;
    xiaotai_wake_status(&status);
    CHECK(status.ready && status.init_error == XIAOTAI_OK && status.resets == 1U);
    s_next_step_us = T0 + 20000;
    s_audio = audio;
    return 0;
}

/* -------------------------------------------------------------------- tests */

static int test_before_init(void)
{
    xiaotai_wake_status_t status;
    setup(NULL, 0);
    xiaotai_wake_status(&status);
    CHECK(status.threshold_milli == 900U && !status.ready && !status.wanted && !status.listening);
    CHECK(status.init_error == XIAOTAI_OK && status.fed == 0U && status.resets == 0U && status.inferences == 0U);
    CHECK(status.detections == 0U && status.last_score_us == 0 && status.detections_lost == 0U);
    CHECK(xiaotai_wake_model() == NULL);
    CHECK(xiaotai_wake_listen(true) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_wake_set_threshold(500) == XIAOTAI_ERR_INVALID_STATE);
    /* The default threshold comes from the tuning table; out of range: 900. */
    static const xiaotai_media_param_t k950[] = {{"wake_engine.threshold_milli", 950}};
    static const xiaotai_media_param_t k1000[] = {{"wake_engine.threshold_milli", 1000}};
    static const xiaotai_media_param_t k49[] = {{"wake_engine.threshold_milli", 49}};
    setup(k950, 1);
    xiaotai_wake_status(&status);
    CHECK(status.threshold_milli == 950 && !status.ready && status.fed == 0);
    setup(k1000, 1);
    xiaotai_wake_status(&status);
    CHECK(status.threshold_milli == 900);
    setup(k49, 1);
    xiaotai_wake_status(&status);
    CHECK(status.threshold_milli == 900);
    return 0;
}

static int test_registration(void)
{
    setup(NULL, 0);
    xiaotai_wake_model_t bad;
    CHECK(xiaotai_wake_model_register(NULL) == XIAOTAI_ERR_INVALID_ARG);
#define REFUSED(field, value)                                                     \
    do {                                                                          \
        bad = g_script.model;                                                     \
        bad.field = value;                                                        \
        CHECK(xiaotai_wake_model_register(&bad) == XIAOTAI_ERR_INVALID_ARG);      \
    } while (0)
    REFUSED(name, NULL);
    REFUSED(open, NULL);
    REFUSED(detect, NULL);
    REFUSED(close, NULL);
    REFUSED(rate_hz, 0);
    REFUSED(window_samples, 0);
    REFUSED(grid_samples, 0);
    REFUSED(stride_samples, 0);
    REFUSED(stack_bytes, 0);
    REFUSED(stride_samples, WINDOW + 1U);
    REFUSED(grid_samples, WINDOW + 1U);
#undef REFUSED
    CHECK(xiaotai_wake_model() == NULL);
    /* The model registered last is used. */
    xiaotai_wake_model_t other = g_script.model;
    other.name = "other";
    CHECK(xiaotai_wake_model_register(&other) == XIAOTAI_OK);
    CHECK(xiaotai_wake_model_register(&g_script.model) == XIAOTAI_OK);
    CHECK(xiaotai_wake_model() == &g_script.model);

    CHECK(xiaotai_wake_subscribe(NULL, NULL) == XIAOTAI_ERR_INVALID_ARG);
    for (unsigned i = 0; i < WAKE_SUBSCRIBERS_MAX; ++i) CHECK(xiaotai_wake_subscribe(subscriber_a, NULL) == XIAOTAI_OK);
    CHECK(xiaotai_wake_subscribe(subscriber_a, NULL) == XIAOTAI_ERR_NO_MEM);

    /* init registers one tap, disabled, hearing only the idle device, after
     * the gain stage. */
    CHECK(xiaotai_engine_wake_engine_init() == XIAOTAI_OK);
    CHECK(xiaotai_engine_wake_engine_init() == XIAOTAI_ERR_INVALID_STATE);
    const xiaotai_media_tap_t *tap = fake_media_tap();
    CHECK(tap && fake_media_tap_registrations() == 1U && !strcmp(tap->name, "wake_engine"));
    CHECK(tap->flags == 0U && tap->gate && tap->pcm && !fake_media_tap_enabled());
    /* Once the task runs the model is fixed. */
    (void)wake_engine_step();
    CHECK(xiaotai_wake_model_register(&other) == XIAOTAI_ERR_INVALID_STATE);
    CHECK(xiaotai_wake_model() == &g_script.model);

    /* wake_engine.raw = 1: the audio before gain control. */
    static const xiaotai_media_param_t raw[] = {{"wake_engine.raw", 1}};
    setup(raw, 1);
    CHECK(xiaotai_engine_wake_engine_init() == XIAOTAI_OK);
    CHECK(fake_media_tap()->flags == XIAOTAI_TAP_RAW);
    return 0;
}

static int test_listen(void)
{
    xiaotai_wake_status_t status;
    xiaotai_media_info_t info;
    /* No model: not supported, the tap stays off; turning off is fine. */
    setup(NULL, 0);
    CHECK(xiaotai_engine_wake_engine_init() == XIAOTAI_OK);
    CHECK(xiaotai_wake_listen(true) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(xiaotai_wake_listen(false) == XIAOTAI_OK);
    CHECK(!fake_media_tap_enabled() && fake_media_tap_enable_calls() == 0U);

    setup(NULL, 0);
    CHECK(xiaotai_wake_model_register(&g_script.model) == XIAOTAI_OK);
    CHECK(xiaotai_engine_wake_engine_init() == XIAOTAI_OK);
    /* No clean PCM (an encoded input), or another capture rate: refused. */
    xiaotai_media_info(&info);
    info.capture_pcm = false;
    fake_media_set_info(&info);
    CHECK(xiaotai_wake_listen(true) == XIAOTAI_ERR_NOT_SUPPORTED);
    info.capture_pcm = true;
    info.capture_rate_hz = 8000;
    fake_media_set_info(&info);
    CHECK(xiaotai_wake_listen(true) == XIAOTAI_ERR_NOT_SUPPORTED);
    CHECK(fake_media_tap_enable_calls() == 0U);
    xiaotai_wake_status(&status);
    CHECK(!status.wanted);
    /* The tap refuses: the error is returned and nothing is wanted. */
    info.capture_rate_hz = 0; /* unknown rate: accepted */
    fake_media_set_info(&info);
    fake_media_fail_enable(XIAOTAI_ERR_NO_MEM);
    CHECK(xiaotai_wake_listen(true) == XIAOTAI_ERR_NO_MEM);
    xiaotai_wake_status(&status);
    CHECK(!status.wanted && !fake_media_tap_enabled());
    /* Wanted while the model still loads; the tap follows each change once. */
    CHECK(xiaotai_wake_listen(true) == XIAOTAI_OK);
    CHECK(fake_media_tap_enabled() && fake_media_tap_enable_calls() == 2U);
    CHECK(xiaotai_wake_listen(true) == XIAOTAI_OK);
    CHECK(fake_media_tap_enable_calls() == 2U);
    xiaotai_wake_status(&status);
    CHECK(status.wanted && !status.ready && !status.listening);
    CHECK(xiaotai_wake_listen(false) == XIAOTAI_OK);
    CHECK(!fake_media_tap_enabled() && fake_media_tap_enable_calls() == 3U);
    xiaotai_wake_status(&status);
    CHECK(!status.wanted);
    return 0;
}

/* Every load failure kind, the self-test (failed, non-finite, then passing
 * with a score that must not count), on the absolute retry schedule, and
 * everything given back after each failure. */
static int test_load_retry(void)
{
    xiaotai_wake_status_t status;
    setup(NULL, 0);
    g_script.open_results[0] = XIAOTAI_ERR_NO_MEM;
    g_script.open_results[1] = XIAOTAI_ERR_INVALID_ARG;
    g_script.open_results[2] = XIAOTAI_ERR_NOT_SUPPORTED;
    g_script.open_results[3] = XIAOTAI_FAIL;
    g_script.open_result_count = 4;
    script(0, (script_step_t){.result = XIAOTAI_FAIL});
    script(1, (script_step_t){.result = XIAOTAI_OK, .probability = NAN});
    script(2, (script_step_t){.result = XIAOTAI_OK, .probability = 1.0f});
    CHECK(xiaotai_wake_model_register(&g_script.model) == XIAOTAI_OK);
    CHECK(xiaotai_engine_wake_engine_init() == XIAOTAI_OK);
    CHECK(xiaotai_wake_subscribe(subscriber_a, NULL) == XIAOTAI_OK);

    const struct {
        int64_t at_us;
        uint32_t wait_ms;
        xiaotai_err_t error;
    } attempts[] = {
        {T0, 5000, XIAOTAI_ERR_NO_MEM},
        {T0 + 5000000, 10000, XIAOTAI_ERR_INVALID_ARG},
        {T0 + 15000000, 20000, XIAOTAI_ERR_NOT_SUPPORTED},
        {T0 + 35000000, 30000, XIAOTAI_FAIL},
        {T0 + 65000000, 30000, XIAOTAI_FAIL},   /* self-test failed */
        {T0 + 95000000, 30000, XIAOTAI_FAIL},   /* self-test score not finite */
        {T0 + 125000000, WAKE_POLL_MS, XIAOTAI_OK},
    };
    for (size_t i = 0; i < sizeof(attempts) / sizeof(attempts[0]); ++i) {
        if (i > 0) {
            /* Not a microsecond early. */
            g_fake_now_us = attempts[i].at_us - 1;
            CHECK(wake_engine_step() == 1U);
            CHECK(g_script.opens == i);
        }
        g_fake_now_us = attempts[i].at_us;
        CHECK(wake_engine_step() == attempts[i].wait_ms);
        CHECK(g_script.opens == i + 1U && g_script.open_at_us[i] == attempts[i].at_us);
        xiaotai_wake_status(&status);
        CHECK(status.init_error == attempts[i].error);
        CHECK(status.ready == (attempts[i].error == XIAOTAI_OK));
        if (attempts[i].error != XIAOTAI_OK) {
            CHECK(xiaotai_mem_host_outstanding() == 0 && !g_script.is_open);
            wake_engine_memory_t memory;
            wake_engine_memory(&memory);
            CHECK(memory.bulk_bytes == 0U);
        }
    }
    /* The self-tests: a silent one-off window (stream 0) of the full length;
     * the models opened for them were closed again. */
    CHECK(g_script.call_count == 3U && g_script.closes == 2U && g_script.open_ok == 3U);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(g_script.calls[i].stream == 0U && g_script.calls[i].end_sample == WINDOW && g_script.calls[i].all_zero);
    }
    /* A self-test score of 1.0 is never a detection and not a scoring. */
    xiaotai_wake_status(&status);
    CHECK(status.detections == 0U && status.inferences == 0U && status.raw_hits == 0U);
    CHECK(fake_owner_posts() == 0U);
    /* What stays taken: the ring (window + grid - 1), the snapshot and the
     * queue, all in bulk memory, plus the model's own state. */
    wake_engine_memory_t memory;
    wake_engine_memory(&memory);
    CHECK(memory.ring_bytes == 32702U && memory.snapshot_bytes == 32384U && memory.queue_bytes == 66056U);
    CHECK(memory.bulk_bytes == 32702U + 32384U + 66056U && memory.internal_bytes == 0U && memory.dma_bytes == 0U);
    CHECK(xiaotai_mem_host_outstanding() == 4);
    xiaotai_wake_reset();
    CHECK(xiaotai_mem_host_outstanding() == 0 && g_script.closes == 3U);
    return 0;
}

/* Each of the engine's own allocations failing: NO_MEM before the model is
 * opened, nothing kept, and the retry 5 s later loads. The fourth allocation
 * is the model's own. */
static int test_alloc_failures(void)
{
    for (uint32_t n = 1; n <= 4; ++n) {
        xiaotai_wake_status_t status;
        setup(NULL, 0);
        CHECK(xiaotai_wake_model_register(&g_script.model) == XIAOTAI_OK);
        CHECK(xiaotai_engine_wake_engine_init() == XIAOTAI_OK);
        xiaotai_mem_host_clear();
        xiaotai_mem_host_fail_nth(n);
        CHECK(wake_engine_step() == 5000U);
        xiaotai_wake_status(&status);
        CHECK(status.init_error == XIAOTAI_ERR_NO_MEM && !status.ready);
        CHECK(g_script.opens == (n <= 3 ? 0U : 1U) && g_script.open_ok == 0U);
        CHECK(xiaotai_mem_host_outstanding() == 0);
        g_fake_now_us = T0 + 5000000;
        CHECK(wake_engine_step() == WAKE_POLL_MS);
        xiaotai_wake_status(&status);
        CHECK(status.ready && status.init_error == XIAOTAI_OK);
        CHECK(xiaotai_mem_host_outstanding() == 4);
    }
    xiaotai_wake_reset();
    CHECK(xiaotai_mem_host_outstanding() == 0);
    return 0;
}

/* Arming, then one scoring per stride on the grid, with the window's content
 * and every scoring counter. */
static int test_cadence(void)
{
    xiaotai_wake_status_t status;
    setup(NULL, 0);
    g_script.default_step = (script_step_t){
        .result = XIAOTAI_OK,
        .probability = 0.25f,
        .timing = {.features_us = 1000, .inference_us = 50000, .head_us = 100, .reused_frames = 16,
                   .computed_frames = 82},
    };
    if (make_ready(true)) return 1;
    /* Armed 4 s after ready: nothing taken before. */
    run_until(T0 + 3999999);
    CHECK(g_script.call_count == 1U);
    xiaotai_wake_status(&status);
    CHECK(status.fed == 0U && status.skipped_unarmed == 124U && !status.listening && status.wanted);
    g_fake_now_us = T0 + 4000000;
    xiaotai_wake_status(&status);
    CHECK(status.listening);

    run_until(T0 + 10000000);
    /* The run starts with frame 124, the first that ends at the arming time. */
    uint64_t k0 = first_frame_from(T0 + 4000000);
    CHECK(k0 == 124U);
    CHECK(g_script.call_count == 33U);
    uint32_t stream = g_script.calls[1].stream;
    CHECK(stream != 0U);
    for (size_t i = 1; i < g_script.call_count; ++i) {
        const detect_call_t *call = &g_script.calls[i];
        uint64_t end = 16352U + 2560U * (i - 1U);
        CHECK(call->stream == stream && call->end_sample == end);
        CHECK((end - WINDOW) % 160U == 0U);
        CHECK(call->at_us == T0 + 5000000 + 160000 * (int64_t)(i - 1U));
        CHECK(call->first == sample(k0 * FRAME + end - WINDOW) && call->last == sample(k0 * FRAME + end - 1U));
    }
    CHECK(window_end_us(k0, 16352) == T0 + 4990000);
    xiaotai_wake_status(&status);
    CHECK(status.inferences == 32U && status.fed == 188U && status.skipped_unarmed == 124U);
    CHECK(status.raw_hits == 0U && status.detections == 0U && status.reject_stream == 0U);
    CHECK(status.reject_age == 0U && status.reject_error == 0U && status.skipped_cooldown == 0U);
    CHECK(status.resets == 1U && status.stale == 0U && status.dropped == 0U && status.wrong_rate == 0U);
    CHECK(status.reused_frames == 16U * 32U && status.computed_frames == 82U * 32U);
    CHECK(status.features_max_us == 1000U && status.inference_max_us == 50000U && status.head_max_us == 100U);
    CHECK(status.detect_avg_us == 0U && status.detect_max_us == 0U && status.detect_over_budget == 0U);
    CHECK(status.window_age_max_ms == 10U);
    CHECK(status.last_score_milli == 250U && status.last_score_us == T0 + 5000000 + 160000 * 31);
    CHECK(status.listening && status.ready && status.threshold_milli == 900U);
    CHECK(fake_owner_posts() == 0U);
    return 0;
}

/* A hit becomes a detection on the owner task, for every subscriber in order;
 * then 2.5 s of cooldown and a new run. */
static int test_detection(void)
{
    xiaotai_wake_status_t status;
    setup(NULL, 0);
    g_script.default_step = (script_step_t){.result = XIAOTAI_OK, .probability = 0.1f};
    script(1, (script_step_t){.result = XIAOTAI_OK, .probability = 0.5f});
    script(2, (script_step_t){.result = XIAOTAI_OK, .probability = 0.9496f});
    if (make_ready(true)) return 1;
    uint64_t k0 = first_frame_from(T0 + 4000000);
    run_until(T0 + 5160000);
    CHECK(g_script.call_count == 3U);
    xiaotai_wake_status(&status);
    CHECK(status.raw_hits == 1U && status.detections == 1U && status.last_score_milli == 949U);
    CHECK(!status.listening);
    /* Posted, not yet run: subscribers hear it on the owner task only. */
    CHECK(fake_owner_posts() == 1U && fake_owner_queued() == 1U && s_seen_count == 0U);
    CHECK(fake_owner_run_all() == 1U);
    CHECK(s_seen_count == 2U && s_seen[0].who == 'a' && s_seen[1].who == 'b');
    for (size_t i = 0; i < 2; ++i) {
        CHECK(s_seen[i].detection.sequence == 1U);
        CHECK(s_seen[i].detection.captured_us == window_end_us(k0, 16352U + 2560U));
        CHECK(s_seen[i].detection.score_milli == 950U && s_seen[i].detection.threshold_milli == 900U);
    }
    /* Cooldown until T0 + 7.66 s: not listening, frames skipped, nothing scored. */
    run_until(T0 + 7659999);
    CHECK(g_script.call_count == 3U);
    xiaotai_wake_status(&status);
    CHECK(!status.listening && status.skipped_unarmed == 124U + 77U && status.resets == 2U);
    g_fake_now_us = T0 + 7660000;
    xiaotai_wake_status(&status);
    CHECK(status.listening);
    /* A new run from the first frame that ends after the cooldown. */
    uint64_t k1 = first_frame_from(T0 + 7660000);
    CHECK(k1 == 239U);
    script(3, (script_step_t){.result = XIAOTAI_OK, .probability = 0.99f});
    run_until(T0 + 8680000);
    CHECK(g_script.call_count == 4U);
    const detect_call_t *call = &g_script.calls[3];
    CHECK(call->at_us == T0 + 8680000 && call->end_sample == 16352U && call->stream != g_script.calls[2].stream);
    CHECK(call->first == sample(k1 * FRAME + 16352U - WINDOW));
    CHECK(fake_owner_run_all() == 1U);
    CHECK(s_seen_count == 4U && s_seen[2].who == 'a' && s_seen[3].who == 'b');
    CHECK(s_seen[2].detection.sequence == 2U && s_seen[2].detection.score_milli == 990U);
    CHECK(s_seen[2].detection.captured_us == window_end_us(k1, 16352U));
    xiaotai_wake_status(&status);
    CHECK(status.detections == 2U && status.raw_hits == 2U && status.detections_lost == 0U);
    return 0;
}

static int test_threshold(void)
{
    xiaotai_wake_status_t status;
    static const xiaotai_media_param_t k950[] = {{"wake_engine.threshold_milli", 950}};
    setup(k950, 1);
    CHECK(xiaotai_wake_model_register(&g_script.model) == XIAOTAI_OK);
    CHECK(xiaotai_engine_wake_engine_init() == XIAOTAI_OK);
    /* Range first, then readiness. */
    CHECK(xiaotai_wake_set_threshold(49) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_wake_set_threshold(991) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_wake_set_threshold(500) == XIAOTAI_ERR_INVALID_STATE);
    xiaotai_wake_reset();
    setup(k950, 1);
    script(1, (script_step_t){.result = XIAOTAI_OK, .probability = 0.9496f});
    script(2, (script_step_t){.result = XIAOTAI_OK, .probability = 0.95f});
    if (make_ready(true)) return 1;
    xiaotai_wake_status(&status);
    CHECK(status.threshold_milli == 950U);
    /* Each change starts a new window. */
    CHECK(xiaotai_wake_set_threshold(50) == XIAOTAI_OK);
    CHECK(xiaotai_wake_set_threshold(990) == XIAOTAI_OK);
    CHECK(xiaotai_wake_set_threshold(49) == XIAOTAI_ERR_INVALID_ARG);
    CHECK(xiaotai_wake_set_threshold(950) == XIAOTAI_OK);
    xiaotai_wake_status(&status);
    CHECK(status.threshold_milli == 950U && status.resets == 4U);
    run_until(T0 + 5160000);
    xiaotai_wake_status(&status);
    CHECK(status.inferences == 2U && status.raw_hits == 1U && status.detections == 1U);
    CHECK(fake_owner_run_all() == 1U);
    CHECK(s_seen_count == 2U && s_seen[0].detection.threshold_milli == 950U && s_seen[0].detection.score_milli == 950U);
    return 0;
}

static int test_score_errors(void)
{
    xiaotai_wake_status_t status;
    setup(NULL, 0);
    script(1, (script_step_t){.result = XIAOTAI_FAIL, .probability = 0.99f,
                              .timing = {.reused_frames = 5, .computed_frames = 93}});
    script(2, (script_step_t){.result = XIAOTAI_OK, .probability = NAN});
    script(3, (script_step_t){.result = XIAOTAI_OK, .probability = INFINITY});
    script(4, (script_step_t){.result = XIAOTAI_OK, .probability = 0.3f,
                              .timing = {.reused_frames = 90, .computed_frames = 8}});
    if (make_ready(true)) return 1;
    run_until(T0 + 5320000);
    xiaotai_wake_status(&status);
    CHECK(status.inferences == 3U && status.reject_error == 3U && status.raw_hits == 0U);
    CHECK(status.last_score_us == 0 && status.last_score_milli == 0U);
    run_until(T0 + 5480000);
    xiaotai_wake_status(&status);
    CHECK(status.inferences == 4U && status.reject_error == 3U && status.detections == 0U);
    CHECK(status.reused_frames == 95U && status.computed_frames == 8U);
    CHECK(status.last_score_milli == 300U && status.last_score_us == T0 + 5480000);
    CHECK(fake_owner_posts() == 0U);
    return 0;
}

static void during_gate_toggle(void)
{
    fake_media_gate(false);
    fake_media_gate(true);
}

static void during_overflow(void)
{
    int16_t pcm[FRAME] = {0};
    for (unsigned i = 0; i < WAKE_QUEUE_BLOCKS + 1U; ++i) fake_media_pcm(pcm, FRAME, 16000, g_fake_now_us);
}

static void during_unlisten(void) { (void)xiaotai_wake_listen(false); }

/* Hits whose stream changed while they were scored: the gate moved, a frame
 * was lost, listening stopped. */
static int test_reject_stream(void)
{
    xiaotai_wake_status_t status;
    setup(NULL, 0);
    g_script.default_step = (script_step_t){.result = XIAOTAI_OK, .probability = 0.1f};
    script(1, (script_step_t){.result = XIAOTAI_OK, .probability = 0.99f, .during = during_gate_toggle});
    script(2, (script_step_t){.result = XIAOTAI_OK, .probability = 0.99f, .during = during_overflow});
    script(3, (script_step_t){.result = XIAOTAI_OK, .probability = 0.99f, .during = during_unlisten});
    if (make_ready(true)) return 1;
    run_until(T0 + 20000000);
    xiaotai_wake_status(&status);
    CHECK(g_script.call_count == 4U);
    CHECK(status.raw_hits == 3U && status.reject_stream == 3U && status.detections == 0U);
    CHECK(status.dropped >= 1U && !status.wanted && !status.listening && !fake_media_tap_enabled());
    /* Each rejection left a new run behind it. */
    CHECK(g_script.calls[2].stream != g_script.calls[1].stream);
    CHECK(g_script.calls[3].stream != g_script.calls[2].stream);
    CHECK(fake_owner_posts() == 0U);
    return 0;
}

/* A hit whose window is more than 1000 ms old when its score is known; the
 * slow scoring also overruns the stride and the queue. */
static int test_reject_age(void)
{
    xiaotai_wake_status_t status;
    setup(NULL, 0);
    script(1, (script_step_t){.result = XIAOTAI_OK, .probability = 0.99f, .duration_us = 1100000});
    if (make_ready(true)) return 1;
    run_until(T0 + 5000000);
    xiaotai_wake_status(&status);
    CHECK(status.raw_hits == 1U && status.reject_age == 1U && status.detections == 0U);
    CHECK(status.detect_over_budget == 1U && status.detect_max_us == 1100000U && status.detect_avg_us == 1100000U);
    CHECK(status.window_age_max_ms == 1110U);
    CHECK(fake_owner_posts() == 0U);
    /* 35 frames came due while scoring and before the next pass 20 ms after it
     * (5.024 s to 6.112 s): 32 fit the queue, 3 were lost; the next pass starts
     * over and drops the 32 queued from before the gap. */
    run_until(T0 + 6120000);
    xiaotai_wake_status(&status);
    CHECK(status.dropped == 3U && status.resets == 2U);
    CHECK(status.fed == 32U && status.skipped_unarmed == 124U + 32U);
    return 0;
}

static int push(int64_t at_us)
{
    for (unsigned i = 0; i < FRAME; ++i) s_pcm[i] = (int16_t)i;
    fake_media_pcm(s_pcm, FRAME, 16000, at_us);
    return 0;
}

/* Gaps, generations, the closed gate and frames that never enter the queue,
 * fed by hand. */
static int test_gaps(void)
{
    xiaotai_wake_status_t status;
    setup(NULL, 0);
    if (make_ready(false)) return 1;
    int64_t at = T0 + 4000000;
    g_fake_now_us = at;
    /* A full queue: the 33rd frame is lost and flags a gap; the next pass
     * starts over and drops what was queued before the gap. */
    for (int i = 0; i < 33; ++i) push(at += FRAME_US);
    xiaotai_wake_status(&status);
    CHECK(status.dropped == 1U);
    g_fake_now_us = at;
    (void)wake_engine_step();
    xiaotai_wake_status(&status);
    CHECK(status.resets == 2U && status.skipped_unarmed == 32U && status.fed == 0U);
    /* Continuity: three regular frames, then one 150 ms later. */
    for (int i = 0; i < 3; ++i) push(at += FRAME_US);
    (void)wake_engine_step();
    xiaotai_wake_status(&status);
    CHECK(status.fed == 3U && status.stale == 0U && status.resets == 2U);
    push(at += 150000);
    (void)wake_engine_step();
    xiaotai_wake_status(&status);
    CHECK(status.fed == 4U && status.stale == 1U && status.resets == 3U);
    /* The gate closing and opening: frames from before are never scored, one
     * new window however often it moved. */
    push(at += FRAME_US);
    push(at += FRAME_US);
    fake_media_gate(false);
    fake_media_gate(true);
    push(at += FRAME_US);
    (void)wake_engine_step();
    xiaotai_wake_status(&status);
    CHECK(status.skipped_unarmed == 34U && status.fed == 5U && status.resets == 4U && status.stale == 1U);
    /* While the gate is closed nothing is queued or counted. */
    fake_media_gate(false);
    push(at += FRAME_US);
    push(at += FRAME_US);
    xiaotai_wake_status(&status);
    CHECK(!status.listening && status.wanted);
    (void)wake_engine_step();
    xiaotai_wake_status(&status);
    CHECK(status.fed == 5U && status.skipped_unarmed == 34U && status.dropped == 1U && status.resets == 5U);
    fake_media_gate(true);
    (void)wake_engine_step();
    xiaotai_wake_status(&status);
    CHECK(status.listening && status.resets == 6U);
    /* Another rate: counted, never queued. */
    fake_media_pcm(s_pcm, FRAME, 8000, at += FRAME_US);
    (void)wake_engine_step();
    xiaotai_wake_status(&status);
    CHECK(status.wrong_rate == 1U && status.fed == 5U && status.dropped == 1U && status.resets == 7U);
    /* A frame larger than a block, or without a capture time, is a loss. */
    int16_t big[WAKE_QUEUE_BLOCK_SAMPLES + 1] = {0};
    fake_media_pcm(big, WAKE_QUEUE_BLOCK_SAMPLES + 1U, 16000, at += FRAME_US);
    fake_media_pcm(s_pcm, FRAME, 16000, 0);
    xiaotai_wake_status(&status);
    CHECK(status.dropped == 3U);
    (void)wake_engine_step();
    xiaotai_wake_status(&status);
    CHECK(status.resets == 8U && status.fed == 5U);
    /* Not wanted: nothing is queued; wanting again is a new window. */
    CHECK(xiaotai_wake_listen(false) == XIAOTAI_OK);
    push(at += FRAME_US);
    (void)wake_engine_step();
    xiaotai_wake_status(&status);
    CHECK(status.fed == 5U && status.skipped_unarmed == 34U && status.resets == 9U && !status.listening);
    CHECK(xiaotai_wake_listen(true) == XIAOTAI_OK);
    push(at += FRAME_US);
    (void)wake_engine_step();
    xiaotai_wake_status(&status);
    CHECK(status.fed == 6U && status.resets == 10U && status.listening);
    CHECK(g_script.call_count == 1U);
    return 0;
}

/* A wrong-rate frame has a timestamp, but contributes no model samples.
 * The adjacent valid frames must not be joined across that hole. */
static int test_wrong_rate_breaks_window(void)
{
    setup(NULL, 0);
    script(1, (script_step_t){.result = XIAOTAI_OK, .probability = 0.99f});
    if (make_ready(false)) return 1;
    int64_t at = T0 + WAKE_ARM_US;
    for (unsigned i = 0; i < 31; ++i) {
        g_fake_now_us = at += FRAME_US;
        push(at);
        (void)wake_engine_step();
    }
    CHECK(g_script.call_count == 1U);
    fake_media_pcm(s_pcm, FRAME, 8000, at += FRAME_US);
    g_fake_now_us = at += FRAME_US;
    push(at); /* 64 ms after the previous valid frame: below the time-gap guard */
    (void)wake_engine_step();
    xiaotai_wake_status_t status;
    xiaotai_wake_status(&status);
    CHECK(g_script.call_count == 1U && status.detections == 0U);
    CHECK(status.wrong_rate == 1U && status.resets == 2U && status.fed == 32U);
    for (unsigned i = 0; i < 31; ++i) {
        g_fake_now_us = at += FRAME_US;
        push(at);
        (void)wake_engine_step();
    }
    xiaotai_wake_status(&status);
    CHECK(g_script.call_count == 2U && status.detections == 1U);
    return 0;
}

/* The owner's queue full when a detection is accepted: kept and posted again
 * at every pass until it fits; after 1000 ms from its window's end it is
 * dropped and counted instead. */
static int test_owner_full(void)
{
    xiaotai_wake_status_t status;
    setup(NULL, 0);
    script(1, (script_step_t){.result = XIAOTAI_OK, .probability = 0.99f});
    fake_owner_set_result(XIAOTAI_ERR_NO_MEM);
    if (make_ready(true)) return 1;
    run_until(T0 + 5000000);
    xiaotai_wake_status(&status);
    CHECK(status.detections == 1U && fake_owner_posts() == 1U && fake_owner_queued() == 0U);
    run_until(T0 + 5500000);
    CHECK(fake_owner_posts() == 1U + 25U && fake_owner_queued() == 0U);
    fake_owner_set_result(XIAOTAI_OK);
    run_until(T0 + 5520000);
    CHECK(fake_owner_posts() == 27U && fake_owner_queued() == 1U);
    CHECK(fake_owner_run_all() == 1U && s_seen_count == 2U && s_seen[0].detection.sequence == 1U);
    CHECK(s_seen[0].detection.captured_us == T0 + 4990000);
    run_until(T0 + 6000000);
    CHECK(fake_owner_posts() == 27U && s_seen_count == 2U);
    xiaotai_wake_status(&status);
    CHECK(status.detections_lost == 0U);

    /* Full for good: the window ended at T0 + 4.99 s; the last try is the pass
     * at 5.98 s (990 ms), the pass at 6.00 s (1010 ms) drops it. */
    setup(NULL, 0);
    script(1, (script_step_t){.result = XIAOTAI_OK, .probability = 0.99f});
    fake_owner_set_result(XIAOTAI_ERR_NO_MEM);
    if (make_ready(true)) return 1;
    run_until(T0 + 5990000);
    xiaotai_wake_status(&status);
    CHECK(status.detections_lost == 0U && fake_owner_posts() == 1U + 49U);
    run_until(T0 + 6000000);
    xiaotai_wake_status(&status);
    CHECK(status.detections_lost == 1U && fake_owner_posts() == 50U);
    fake_owner_set_result(XIAOTAI_OK);
    run_until(T0 + 6100000);
    CHECK(fake_owner_posts() == 50U && fake_owner_queued() == 0U && s_seen_count == 0U);
    return 0;
}

/* The owner never runs: four posted detections wait in order; later ones
 * are not posted and age out. */
static int test_posted_ring(void)
{
    xiaotai_wake_status_t status;
    setup(NULL, 0);
    g_script.default_step = (script_step_t){.result = XIAOTAI_OK, .probability = 0.99f};
    if (make_ready(true)) return 1;
    /* Accepted at 5.00, 8.52, 12.04, 15.56, 19.08, 22.60 and 26.12 s (cooldown,
     * then 32 frames, then the next pass); the last three age out by 27.12 s. */
    run_until(T0 + 29000000);
    xiaotai_wake_status(&status);
    CHECK(status.detections == 7U && status.detections_lost == 3U);
    CHECK(fake_owner_queued() == WAKE_POSTED_MAX && fake_owner_posts() == WAKE_POSTED_MAX);
    CHECK(fake_owner_run_all() == WAKE_POSTED_MAX);
    CHECK(s_seen_count == 2U * WAKE_POSTED_MAX);
    for (uint32_t i = 0; i < WAKE_POSTED_MAX; ++i) {
        CHECK(s_seen[2 * i].who == 'a' && s_seen[2 * i + 1].who == 'b');
        CHECK(s_seen[2 * i].detection.sequence == i + 1U && s_seen[2 * i + 1].detection.sequence == i + 1U);
    }
    return 0;
}

/* The real task on the host port and the real clock: created as tuned, loads
 * the model, and stops for reset with everything given back. */
static int test_task(void)
{
    xiaotai_wake_status_t status;
    xiaotai_task_config_t config;
    static const xiaotai_media_param_t tuned[] = {{"wake_engine.task_priority", 5}, {"wake_engine.task_core", 1}};
    setup(tuned, 2);
    wake_engine_set_clock(NULL);
    /* Without a model start creates nothing. */
    CHECK(xiaotai_engine_wake_engine_init() == XIAOTAI_OK);
    xiaotai_engine_wake_engine_start();
    wake_engine_task_config(&config);
    CHECK(config.entry == NULL && config.stack_bytes == 0U);
    xiaotai_wake_status(&status);
    CHECK(!status.ready && status.init_error == XIAOTAI_OK);

    setup(tuned, 2);
    wake_engine_set_clock(NULL);
    CHECK(xiaotai_wake_model_register(&g_script.model) == XIAOTAI_OK);
    CHECK(xiaotai_engine_wake_engine_init() == XIAOTAI_OK);
    xiaotai_engine_wake_engine_start();
    xiaotai_engine_wake_engine_start(); /* once */
    wake_engine_task_config(&config);
    CHECK(!strcmp(config.name, "xt_wake") && config.entry != NULL);
    CHECK(config.stack_bytes == 12288U && config.stack == XIAOTAI_MEM_BULK);
    CHECK(config.priority == 5U && config.core == 1);
    CHECK(xiaotai_wake_model_register(&g_script.model) == XIAOTAI_ERR_INVALID_STATE);
    for (int i = 0; i < 400; ++i) {
        xiaotai_wake_status(&status);
        if (status.ready) break;
        xiaotai_delay_ms(5);
    }
    CHECK(status.ready && status.init_error == XIAOTAI_OK && g_script.opens == 1U);
    xiaotai_wake_reset();
    CHECK(g_script.closes == 1U && xiaotai_mem_host_outstanding() == 0);

    /* Omitting the deployed zero entries must keep the tap after gain and
     * the wake task on core 0, with no change to the default priority. */
    static const xiaotai_media_param_t deployed[] = {{"wake_engine.raw", 0}, {"wake_engine.task_core", 0}};
    for (unsigned configured = 0; configured < 2; ++configured) {
        setup(configured ? deployed : NULL, configured ? sizeof(deployed) / sizeof(deployed[0]) : 0);
        wake_engine_set_clock(NULL);
        CHECK(xiaotai_wake_model_register(&g_script.model) == XIAOTAI_OK);
        CHECK(xiaotai_engine_wake_engine_init() == XIAOTAI_OK);
        const xiaotai_media_tap_t *tap = fake_media_tap();
        CHECK(tap && tap->flags == 0U);
        xiaotai_engine_wake_engine_start();
        wake_engine_task_config(&config);
        CHECK(config.entry && config.priority == 3U && config.core == 0);
        xiaotai_wake_reset();
        CHECK(xiaotai_mem_host_outstanding() == 0);
    }
    return 0;
}

int main(void)
{
    static int (*const tests[])(void) = {
        test_before_init, test_registration, test_listen,       test_load_retry,  test_alloc_failures,
        test_cadence,     test_detection,    test_threshold,    test_score_errors, test_reject_stream,
        test_reject_age,  test_gaps,         test_wrong_rate_breaks_window, test_owner_full, test_posted_ring, test_task,
    };
    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i) {
        if (tests[i]()) {
            fprintf(stderr, "wake_loop: test %zu failed\n", i);
            return 1;
        }
    }
    xiaotai_wake_reset();
    puts("wake_loop: ok");
    return 0;
}
