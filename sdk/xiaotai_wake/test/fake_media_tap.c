// SPDX-License-Identifier: MIT
/* Fakes of what the wake engine calls outside itself: the media engine's tap
 * registry, info and tuning table, and the framework's xiaotai_run. The test
 * plays the capture task (fake_media_gate, fake_media_pcm) and the owner task
 * (fake_owner_run_all) itself, on one thread. */
#include <string.h>
#include "fakes.h"
#include "internal.h"

int64_t g_fake_now_us;

int64_t fake_now(void) { return g_fake_now_us; }

/* ------------------------------------------------------------------- media */

static const xiaotai_media_tap_t *s_tap;
static unsigned s_registrations, s_enable_calls;
static bool s_enabled;
static xiaotai_err_t s_enable_failure;
static xiaotai_media_info_t s_info;
static const xiaotai_media_param_t *s_params;
static size_t s_param_count;

void fake_media_reset(void)
{
    s_tap = NULL;
    s_registrations = s_enable_calls = 0;
    s_enabled = false;
    s_enable_failure = XIAOTAI_OK;
    memset(&s_info, 0, sizeof(s_info));
    s_info.capture = s_info.capture_pcm = true;
    s_info.capture_encoding = XIAOTAI_CODEC_PCM16;
    s_info.capture_rate_hz = 16000;
    s_info.processor = "fake";
    s_params = NULL;
    s_param_count = 0;
}

void fake_media_set_info(const xiaotai_media_info_t *info) { s_info = *info; }

void fake_media_set_params(const xiaotai_media_param_t *params, size_t count)
{
    s_params = params;
    s_param_count = count;
}

void fake_media_fail_enable(xiaotai_err_t err) { s_enable_failure = err; }
const xiaotai_media_tap_t *fake_media_tap(void) { return s_tap; }
unsigned fake_media_tap_registrations(void) { return s_registrations; }
bool fake_media_tap_enabled(void) { return s_enabled; }
unsigned fake_media_tap_enable_calls(void) { return s_enable_calls; }

void fake_media_gate(bool open)
{
    if (s_tap && s_tap->gate) s_tap->gate(s_tap->ctx, open);
}

void fake_media_pcm(const int16_t *pcm, size_t samples, uint32_t rate_hz, int64_t captured_us)
{
    if (s_tap && s_tap->pcm) s_tap->pcm(s_tap->ctx, pcm, samples, rate_hz, captured_us);
}

xiaotai_err_t xiaotai_media_tap_register(const xiaotai_media_tap_t *tap)
{
    if (!tap || !tap->name || !tap->pcm) return XIAOTAI_ERR_INVALID_ARG;
    s_tap = tap;
    ++s_registrations;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_media_tap_enable(const xiaotai_media_tap_t *tap, bool enabled)
{
    ++s_enable_calls;
    if (!tap || tap != s_tap) return XIAOTAI_ERR_NOT_FOUND;
    if (s_enable_failure != XIAOTAI_OK) {
        xiaotai_err_t err = s_enable_failure;
        s_enable_failure = XIAOTAI_OK;
        return err;
    }
    s_enabled = enabled;
    return XIAOTAI_OK;
}

void xiaotai_media_info(xiaotai_media_info_t *info) { *info = s_info; }

int32_t xiaotai_media_param(const char *name, int32_t fallback)
{
    /* A later entry overrides an earlier one, as with several tables. */
    int32_t value = fallback;
    for (size_t i = 0; i < s_param_count; ++i) {
        if (!strcmp(s_params[i].name, name)) value = s_params[i].value;
    }
    return value;
}

/* --------------------------------------------------------------- framework */

#define OWNER_QUEUE 16U

static struct {
    void (*fn)(void *);
    void *ctx;
} s_jobs[OWNER_QUEUE];
static unsigned s_job_count, s_posts;
static xiaotai_err_t s_run_result;

void fake_owner_reset(void)
{
    memset(s_jobs, 0, sizeof(s_jobs));
    s_job_count = s_posts = 0;
    s_run_result = XIAOTAI_OK;
}

void fake_owner_set_result(xiaotai_err_t result) { s_run_result = result; }
unsigned fake_owner_posts(void) { return s_posts; }
unsigned fake_owner_queued(void) { return s_job_count; }

unsigned fake_owner_run_all(void)
{
    unsigned ran = 0;
    while (ran < s_job_count) {
        s_jobs[ran].fn(s_jobs[ran].ctx);
        ++ran;
    }
    s_job_count = 0;
    return ran;
}

xiaotai_err_t xiaotai_run(void (*fn)(void *ctx), void *ctx)
{
    ++s_posts;
    if (s_run_result != XIAOTAI_OK) return s_run_result;
    if (s_job_count == OWNER_QUEUE) return XIAOTAI_ERR_NO_MEM;
    s_jobs[s_job_count].fn = fn;
    s_jobs[s_job_count].ctx = ctx;
    ++s_job_count;
    return XIAOTAI_OK;
}

/* The diagnostics fixture uses the real framework and console. */
xiaotai_err_t wake_diagnostics_register(void) { return XIAOTAI_OK; }
