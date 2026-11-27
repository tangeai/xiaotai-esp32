// SPDX-License-Identifier: MIT
#ifndef XIAOTAI_MEDIA_SESSION_H
#define XIAOTAI_MEDIA_SESSION_H

#include "xiaotai/media.h"

typedef enum {
    MEDIA_SESSION_IDLE, MEDIA_SESSION_ARMED, MEDIA_SESSION_ACTIVE,
    MEDIA_SESSION_FINISHING,    /* the far end is done; its last sound may still come */
    MEDIA_SESSION_DRAINING, MEDIA_SESSION_DRAINED, MEDIA_SESSION_FAILED,
} media_session_phase_t;

/* Shared state: the caller serializes access. Only the task that owns a part
 * performs hardware operations; it reports the completed generation here. */
typedef struct {
    xiaotai_media_session_t config;
    media_session_phase_t phase;
    uint32_t previous, stale_calls;
    int64_t drain_due_us;
    int64_t tail_open_us, tail_due_us, tail_quiet_us, tail_quiet_from_us;
    uint32_t tail_received;     /* packets received when the far end said it was done */
    bool start_pending, uplink_open, preroll_bound;
} media_session_control_t;

xiaotai_err_t media_session_control_open(media_session_control_t *control,
                                        const xiaotai_media_session_t *session, bool preroll_valid, bool mic_muted);
xiaotai_err_t media_session_control_activate(media_session_control_t *control, uint32_t generation,
                                            bool preroll_valid, bool mic_muted);
xiaotai_err_t media_session_control_drain(media_session_control_t *control, uint32_t generation,
                                         uint32_t timeout_ms, int64_t now_us);
xiaotai_err_t media_session_control_finish(media_session_control_t *control, uint32_t generation,
                                          const xiaotai_media_tail_t *tail, uint32_t received, int64_t now_us);
/* What was held has just been thrown away, the time of the last packet with it:
 * the quiet of a tail is counted from here. */
void media_session_control_quiet_from(media_session_control_t *control, int64_t now_us);
/* True once: the tail is over and the drain began. heard_us: when everything
 * received so far has been heard. last_packet_us 0: none came. */
bool media_session_control_tail_over(media_session_control_t *control, uint32_t generation, int64_t heard_us,
                                     int64_t last_packet_us, int64_t now_us);
bool media_session_control_close(media_session_control_t *control, uint32_t generation);
bool media_session_control_matches(media_session_control_t *control, uint32_t generation);
bool media_session_control_start_result(media_session_control_t *control, uint32_t generation,
                                        xiaotai_err_t result);
bool media_session_control_drain_result(media_session_control_t *control, uint32_t generation, bool empty,
                                        int64_t now_us, xiaotai_err_t *result);

#endif
