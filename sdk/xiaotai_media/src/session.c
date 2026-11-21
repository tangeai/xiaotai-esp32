// SPDX-License-Identifier: MIT
#include "session.h"
#include "internal.h"

bool media_session_control_matches(media_session_control_t *control, uint32_t generation)
{
    if (generation && control->config.generation == generation) return true;
    ++control->stale_calls;
    return false;
}

static void activate(media_session_control_t *control, bool mic_muted)
{
    control->phase = MEDIA_SESSION_ACTIVE;
    control->start_pending = true;
    control->uplink_open = control->config.uplink.rate_hz && control->config.duplex && !mic_muted;
    control->preroll_bound = control->config.preroll_token != 0;
}

xiaotai_err_t media_session_control_open(media_session_control_t *control,
                                        const xiaotai_media_session_t *session, bool preroll_valid, bool mic_muted)
{
    const xiaotai_err_t err = media_session_validate(session);
    if (err != XIAOTAI_OK) return err;
    if (control->phase != MEDIA_SESSION_IDLE || session->generation == control->previous ||
        (session->preroll_token && (mic_muted || !preroll_valid))) return XIAOTAI_ERR_INVALID_STATE;

    control->config = *session;
    control->phase = MEDIA_SESSION_ARMED;
    if (!(session->flags & XIAOTAI_MEDIA_HOLD)) activate(control, mic_muted);
    return XIAOTAI_OK;
}

xiaotai_err_t media_session_control_activate(media_session_control_t *control, uint32_t generation,
                                            bool preroll_valid, bool mic_muted)
{
    if (!media_session_control_matches(control, generation) || control->phase != MEDIA_SESSION_ARMED ||
        (control->config.preroll_token && (mic_muted || !preroll_valid))) return XIAOTAI_ERR_INVALID_STATE;
    activate(control, mic_muted);
    return XIAOTAI_OK;
}

xiaotai_err_t media_session_control_drain(media_session_control_t *control, uint32_t generation,
                                         uint32_t timeout_ms, int64_t now_us)
{
    if (!media_session_control_matches(control, generation) || control->phase != MEDIA_SESSION_ACTIVE)
        return XIAOTAI_ERR_INVALID_STATE;
    if (timeout_ms > XIAOTAI_MEDIA_DRAIN_TIMEOUT_MS) timeout_ms = XIAOTAI_MEDIA_DRAIN_TIMEOUT_MS;
    const int64_t delay_us = (int64_t)timeout_ms * 1000;
    control->drain_due_us = now_us <= INT64_MAX - delay_us ? now_us + delay_us : INT64_MAX;
    control->phase = MEDIA_SESSION_DRAINING;
    control->uplink_open = false;
    return XIAOTAI_OK;
}

bool media_session_control_close(media_session_control_t *control, uint32_t generation)
{
    if (!media_session_control_matches(control, generation)) return false;
    *control = (media_session_control_t){.previous = generation, .stale_calls = control->stale_calls};
    return true;
}

bool media_session_control_start_result(media_session_control_t *control, uint32_t generation,
                                        xiaotai_err_t result)
{
    if (!control->start_pending || control->config.generation != generation) return false;
    control->start_pending = false;
    if (result != XIAOTAI_OK) {
        control->phase = MEDIA_SESSION_FAILED;
        control->uplink_open = false;
    }
    return true;
}

bool media_session_control_drain_result(media_session_control_t *control, uint32_t generation, bool empty,
                                        int64_t now_us, xiaotai_err_t *result)
{
    if (control->phase != MEDIA_SESSION_DRAINING || control->start_pending ||
        control->config.generation != generation || (!empty && now_us < control->drain_due_us)) return false;
    *result = empty ? XIAOTAI_OK : XIAOTAI_ERR_TIMEOUT;
    control->phase = MEDIA_SESSION_DRAINED;
    return true;
}
