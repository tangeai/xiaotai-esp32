// SPDX-License-Identifier: MIT
#ifndef XIAOTAI_MEDIA_SESSION_EVENTS_H
#define XIAOTAI_MEDIA_SESSION_EVENTS_H

#include "xiaotai/media.h"
#include "xiaotai/port/os.h"

#define MEDIA_SESSION_EVENTS (XIAOTAI_MEDIA_EVENT_PREROLL_LOST + 1U)

typedef struct {
    xiaotai_critical_t lock;
    uint32_t generation, pending, delivered, retries;
    uint8_t order[MEDIA_SESSION_EVENTS], head, count;
    bool posted;
    xiaotai_err_t error[MEDIA_SESSION_EVENTS];
    xiaotai_media_event_fn_t callback;
    void *ctx;
} media_session_events_t;

void media_session_events_init(media_session_events_t *events);
/* Owner only: close clears the generation before its callback context is released. */
void media_session_events_bind(media_session_events_t *events, const xiaotai_media_session_t *session);
/* Non-blocking from the part's owner. Feed real hardware completion, not a queued request. */
void media_session_events_record(media_session_events_t *events, uint32_t generation,
                                 xiaotai_media_event_t event, xiaotai_err_t error);
void media_session_events_post(media_session_events_t *events);
bool media_session_events_pending(media_session_events_t *events);
uint32_t media_session_events_retries(media_session_events_t *events);

#endif
