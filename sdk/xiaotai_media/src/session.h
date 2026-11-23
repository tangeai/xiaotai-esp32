// SPDX-License-Identifier: MIT
#ifndef XIAOTAI_MEDIA_SESSION_H
#define XIAOTAI_MEDIA_SESSION_H

#include "xiaotai/media.h"

typedef enum {
    MEDIA_SESSION_IDLE, MEDIA_SESSION_ARMED, MEDIA_SESSION_ACTIVE,
    MEDIA_SESSION_DRAINING, MEDIA_SESSION_DRAINED, MEDIA_SESSION_FAILED,
} media_session_phase_t;

/* Shared state: the caller serializes access. Only the task that owns a part
 * performs hardware operations; it reports the completed generation here. */
typedef struct {
    xiaotai_media_session_t config;
    media_session_phase_t phase;
    uint32_t previous, stale_calls;
    int64_t drain_due_us;
    bool start_pending, uplink_open, preroll_bound;
} media_session_control_t;

xiaotai_err_t media_session_control_open(media_session_control_t *control,
                                        const xiaotai_media_session_t *session, bool preroll_valid, bool mic_muted);
xiaotai_err_t media_session_control_activate(media_session_control_t *control, uint32_t generation,
                                            bool preroll_valid, bool mic_muted);
xiaotai_err_t media_session_control_drain(media_session_control_t *control, uint32_t generation,
                                         uint32_t timeout_ms, int64_t now_us);
bool media_session_control_close(media_session_control_t *control, uint32_t generation);
bool media_session_control_matches(media_session_control_t *control, uint32_t generation);
bool media_session_control_start_result(media_session_control_t *control, uint32_t generation,
                                        xiaotai_err_t result);
bool media_session_control_drain_result(media_session_control_t *control, uint32_t generation, bool empty,
                                        int64_t now_us, xiaotai_err_t *result);

#endif
