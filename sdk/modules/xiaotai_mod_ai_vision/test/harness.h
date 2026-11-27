// SPDX-License-Identifier: MIT
#pragma once
/* Shared harness of the AI vision module's host tests: the real framework,
 * port and event log; the module compiled from its source; fakes of the video
 * engine and of the transport's permission call, which record every call in
 * one ordered trace; and a host module that begins the activity "ai" and sets
 * its generation as the AI talk module does, plus an activity "call" of higher
 * priority that preempts it.
 *
 * Fakes and module state are touched on the framework owner task only: the
 * test thread acts through the owner_* helpers, which run on the owner task
 * and wait until everything queued before them (events included) has run. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "xiaotai/framework.h"
#include "xiaotai/link.h"
#include "xiaotai/modules/ai_vision.h"
#include "xiaotai/video.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            fprintf(stderr, "trace: %s\n", trace);                                        \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

/* ------------------------------------------------------------ fake engines */

typedef struct {
    /* What xiaotai_video_capabilities() reports. */
    xiaotai_video_capabilities_t capabilities;
    /* Results; XIAOTAI_OK unless the test sets another. */
    xiaotai_err_t resolve_result, require_result, open_result;
    unsigned capability_reads, resolves, requires, opens, closes;
    xiaotai_video_profile_t resolved;       /* profile of the last resolve */
    size_t resolved_camera;
    xiaotai_video_profile_t required;       /* profile of the last declaration */
    xiaotai_video_session_t session;        /* last session opened, pointers as given */
    xiaotai_video_profile_t opened;         /* copy of its uplink profile */
    bool opened_downlink;                   /* it named a downlink */
} fake_video_t;
extern fake_video_t fake_video;

/* The link object xiaotai_video_transport_link() returns. */
extern const xiaotai_video_link_t fake_transport_link;

typedef struct {
    xiaotai_err_t grant_result;             /* returned for a grant; withdrawals return XIAOTAI_OK */
    unsigned permits;
} fake_transport_t;
extern fake_transport_t fake_transport;

/* Calls in order, each followed by a space:
 *   permit:<gen>:<kind>:<0|1>   xiaotai_link_permit
 *   require                     xiaotai_video_require_uplink
 *   open:<gen>                  xiaotai_video_open
 *   close:<gen>                 xiaotai_video_close
 *   revoked                     the host's activity "ai" was revoked
 *   state:<n>                   "ai_vision.state" with value n reached a subscriber */
extern char trace[2048];
void trace_clear(void);
/* Times word appears in the trace. */
int trace_count(const char *word);

/* "ai_vision.state" events in dispatch order; a payload that is not a status
 * of that state is recorded as -1. */
#define STATE_EVENTS_MAX 64
extern int state_events[STATE_EVENTS_MAX];
extern size_t state_event_count;
extern xiaotai_ai_vision_status_t last_state_payload;

/* Resets the fakes to a camera with a JPEG encoder (and an H.264 one) and
 * every call succeeding. */
void fakes_reset(void);

/* ---------------------------------------------------------------- harness */

/* Starts the framework with the AI vision module (configured with config
 * unless it is NULL, which leaves the module unconfigured) and the host
 * module. clear_storage forgets every stored value first; false keeps what an
 * earlier start stored (a reboot). */
int harness_start(const xiaotai_ai_vision_config_t *config, bool clear_storage);
/* Starts the framework with the host module only: a product without vision. */
int harness_start_without_vision(void);
/* Stops the framework and forgets the module's state (and configuration). */
void harness_stop(void);

/* Runs fn(ctx) on the owner task and waits until it and everything queued
 * before it ran. */
void on_owner(void (*fn)(void *ctx), void *ctx);
/* Waits until everything queued so far has run. */
void drain(void);

/* Owner-task actions of the host and the engines. */
uint32_t owner_ai_begin(void);                          /* activity "ai" with generation 0; its id, 0 when refused */
uint32_t owner_ai_begin_at(uint32_t generation);        /* the same, its generation known at the start */
void owner_ai_generation(uint32_t id, uint32_t generation);
void owner_ai_end(uint32_t id);
uint32_t owner_call_begin(void);                        /* activity "call", priority above "ai" */
void owner_call_end(uint32_t id);
uint32_t owner_current_activity(void);
uint32_t owner_activity_generation(uint32_t id);
/* The video engine emits "video.uplink" for generation with phase and error. */
void owner_uplink(uint32_t generation, xiaotai_video_phase_t phase, xiaotai_err_t error);
xiaotai_err_t owner_set_enabled(bool enabled);
xiaotai_ai_vision_status_t owner_status(void);
/* xiaotai_activity_contributions() on the owner task. */
size_t owner_contributions(const char *activity, const char *slot, const char *scene, const void **out,
                           size_t capacity);
/* xiaotai_setting_set_int() on the owner task (what AT+SET does). */
xiaotai_err_t owner_setting_set(const char *name, int32_t value);
/* Posts the intent "vision.enable" with value and waits until it was handled. */
xiaotai_err_t post_vision_intent(int32_t value);
/* The diagnostics section "ai_vision" as one line. */
const char *owner_diag(void);
/* Quiesces the system (true) or releases the quiesce (false). */
void owner_quiesce(bool quiesced);
/* Events the module recorded in the event log (source "ai_vision") with this
 * label, newest last; the newest one's generation, value and level. */
int eventlog_count(const char *label);
bool eventlog_last(const char *label, uint32_t *generation, int32_t *value, uint8_t *level);
