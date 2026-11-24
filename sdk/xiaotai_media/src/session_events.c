// SPDX-License-Identifier: MIT
#include "session_events.h"

#include "xiaotai/framework.h"

void media_session_events_init(media_session_events_t *events)
{
    *events = (media_session_events_t){0};
    xiaotai_critical_init(&events->lock);
}

void media_session_events_bind(media_session_events_t *events, const xiaotai_media_session_t *session)
{
    xiaotai_critical_enter(&events->lock);
    events->generation = session ? session->generation : 0;
    events->callback = session ? session->event : NULL;
    events->ctx = session ? session->ctx : NULL;
    events->pending = events->delivered = 0;
    events->head = events->count = 0;
    /* A trampoline already in the owner queue still owns its slot. */
    xiaotai_critical_exit(&events->lock);
}

void media_session_events_record(media_session_events_t *events, uint32_t generation,
                                 xiaotai_media_event_t event, xiaotai_err_t error)
{
    if (!generation || (unsigned)event >= MEDIA_SESSION_EVENTS) return;
    const uint32_t bit = UINT32_C(1) << (unsigned)event;
    xiaotai_critical_enter(&events->lock);
    if (events->generation == generation && events->callback && !(events->delivered & bit)) {
        if (!(events->pending & bit)) {
            const unsigned tail = (events->head + events->count) % MEDIA_SESSION_EVENTS;
            events->order[tail] = (uint8_t)event;
            ++events->count;
        }
        events->error[event] = error;
        events->pending |= bit;
    }
    xiaotai_critical_exit(&events->lock);
}

static void deliver(void *ctx)
{
    media_session_events_t *events = ctx;
    for (unsigned count = 0; count < MEDIA_SESSION_EVENTS; ++count) {
        xiaotai_critical_enter(&events->lock);
        if (!events->pending) {
            events->posted = false;
            xiaotai_critical_exit(&events->lock);
            return;
        }
        const unsigned index = events->order[events->head];
        events->head = (uint8_t)((events->head + 1U) % MEDIA_SESSION_EVENTS);
        --events->count;
        const uint32_t bit = UINT32_C(1) << index;
        events->pending &= ~bit;
        events->delivered |= bit;
        const uint32_t generation = events->generation;
        const xiaotai_err_t result = events->error[index];
        const xiaotai_media_event_fn_t callback = events->callback;
        void *const userdata = events->ctx;
        xiaotai_critical_exit(&events->lock);
        callback(generation, (xiaotai_media_event_t)index, result, userdata);
    }
    xiaotai_critical_enter(&events->lock);
    events->posted = false;
    xiaotai_critical_exit(&events->lock);
}

void media_session_events_post(media_session_events_t *events)
{
    xiaotai_critical_enter(&events->lock);
    const bool post = events->pending && !events->posted;
    if (post) events->posted = true;
    xiaotai_critical_exit(&events->lock);
    if (!post) return;
    const xiaotai_err_t err = xiaotai_run(deliver, events);
    if (err == XIAOTAI_OK) return;
    xiaotai_critical_enter(&events->lock);
    events->posted = false;
    if (err == XIAOTAI_ERR_NO_MEM) ++events->retries;
    xiaotai_critical_exit(&events->lock);
}

bool media_session_events_pending(media_session_events_t *events)
{
    xiaotai_critical_enter(&events->lock);
    const bool pending = events->pending != 0 && !events->posted;
    xiaotai_critical_exit(&events->lock);
    return pending;
}

uint32_t media_session_events_retries(media_session_events_t *events)
{
    xiaotai_critical_enter(&events->lock);
    const uint32_t retries = events->retries;
    xiaotai_critical_exit(&events->lock);
    return retries;
}
