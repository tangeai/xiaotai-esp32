// SPDX-License-Identifier: MIT
#include <assert.h>
#include <stdatomic.h>
#include "session_events.h"
#include "xiaotai/framework.h"

#define WAIT_MS 3000U

static media_session_events_t events;
static xiaotai_signal_t reached, release, received;
static atomic_uint callbacks;
static xiaotai_media_event_t kinds[3];
static uint32_t generations[3];
static xiaotai_err_t results[3];

static void event(uint32_t generation, xiaotai_media_event_t kind, xiaotai_err_t result, void *ctx)
{
    assert(xiaotai_on_owner() && ctx == &events);
    const unsigned index = atomic_load(&callbacks);
    assert(index < 3);
    kinds[index] = kind;
    generations[index] = generation;
    results[index] = result;
    atomic_store(&callbacks, index + 1);
    if (index > 0) xiaotai_signal_give(&received);
}

static void bind(void *arg)
{
    const xiaotai_media_session_t *session = arg;
    media_session_events_bind(&events, session);
    xiaotai_signal_give(&reached);
}

static void give(void *arg) { xiaotai_signal_give(arg); }
static void skip(void *arg) { (void)arg; }

static void hold(void *arg)
{
    (void)arg;
    xiaotai_signal_give(&reached);
    assert(xiaotai_signal_take(&release, WAIT_MS));
}

int main(void)
{
    assert(xiaotai_signal_init(&reached) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&release) == XIAOTAI_OK);
    assert(xiaotai_signal_init(&received) == XIAOTAI_OK);
    media_session_events_init(&events);
    const xiaotai_framework_config_t framework = {.queue_depth = 2};
    assert(xiaotai_framework_start(&framework) == XIAOTAI_OK);

    xiaotai_media_session_t session = {.generation = 21, .event = event, .ctx = &events};
    assert(xiaotai_run(bind, &session) == XIAOTAI_OK && xiaotai_signal_take(&reached, WAIT_MS));
    assert(xiaotai_run(hold, NULL) == XIAOTAI_OK && xiaotai_signal_take(&reached, WAIT_MS));
    assert(xiaotai_run(skip, NULL) == XIAOTAI_OK);
    assert(xiaotai_run(give, &reached) == XIAOTAI_OK);
    media_session_events_record(&events, 21, XIAOTAI_MEDIA_EVENT_STARTED, XIAOTAI_OK);
    media_session_events_record(&events, 21, XIAOTAI_MEDIA_EVENT_STARTED, XIAOTAI_OK);
    media_session_events_record(&events, 20, XIAOTAI_MEDIA_EVENT_FAILED, XIAOTAI_FAIL);
    media_session_events_record(&events, 21, XIAOTAI_MEDIA_EVENT_DRAINED, XIAOTAI_ERR_TIMEOUT);
    media_session_events_post(&events);
    media_session_events_post(&events);
    assert(media_session_events_retries(&events) == 2 && !atomic_load(&callbacks));
    xiaotai_signal_give(&release);
    assert(xiaotai_signal_take(&reached, WAIT_MS));
    media_session_events_post(&events);
    assert(xiaotai_signal_take(&received, WAIT_MS));
    assert(atomic_load(&callbacks) == 2);
    assert(generations[0] == 21 && kinds[0] == XIAOTAI_MEDIA_EVENT_STARTED && results[0] == XIAOTAI_OK);
    assert(generations[1] == 21 && kinds[1] == XIAOTAI_MEDIA_EVENT_DRAINED && results[1] == XIAOTAI_ERR_TIMEOUT);

    assert(xiaotai_run(hold, NULL) == XIAOTAI_OK && xiaotai_signal_take(&reached, WAIT_MS));
    assert(xiaotai_run(bind, NULL) == XIAOTAI_OK);
    media_session_events_record(&events, 21, XIAOTAI_MEDIA_EVENT_STARTED, XIAOTAI_OK);
    media_session_events_record(&events, 21, XIAOTAI_MEDIA_EVENT_PREROLL_LOST, XIAOTAI_FAIL);
    media_session_events_post(&events);
    xiaotai_signal_give(&release);
    assert(xiaotai_signal_take(&reached, WAIT_MS));
    session.generation = 22;
    assert(xiaotai_run(bind, &session) == XIAOTAI_OK && xiaotai_signal_take(&reached, WAIT_MS));
    assert(atomic_load(&callbacks) == 2);
    media_session_events_record(&events, 22, XIAOTAI_MEDIA_EVENT_FAILED, XIAOTAI_ERR_INVALID_STATE);
    media_session_events_record(&events, 22, XIAOTAI_MEDIA_EVENT_FAILED, XIAOTAI_FAIL);
    media_session_events_post(&events);
    assert(xiaotai_signal_take(&received, WAIT_MS));
    assert(atomic_load(&callbacks) == 3);
    assert(generations[2] == 22 && kinds[2] == XIAOTAI_MEDIA_EVENT_FAILED && results[2] == XIAOTAI_FAIL);

    xiaotai_framework_reset();
    xiaotai_signal_deinit(&received);
    xiaotai_signal_deinit(&release);
    xiaotai_signal_deinit(&reached);
    return 0;
}
