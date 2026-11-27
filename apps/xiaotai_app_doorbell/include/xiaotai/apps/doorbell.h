// SPDX-License-Identifier: MIT
#pragma once
/* Application "doorbell": the door station of one household's doorbell
 * (门铃室外机) on a device without a display. A key the visitor presses, a
 * speaker and a microphone (both may be silent, xiaotai/parts/audio.h
 * xiaotai_audio_silent_create), lights, and with the video_call module the
 * camera the indoor station sees. It speaks the door application's call
 * messages (the same type values as xiaotai/apps/door.h), so that
 * application's indoor stations (门禁室内机, 可视门铃) ring for it, look at
 * it, talk to the visitor and open the door; nothing changes on their side.
 *
 * The key ("main", one click):
 *   idle                  ring the household's indoor stations in turn, each
 *                         for ring_ms ("正在呼叫" and the ring-back);
 *   ringing               stop ringing;
 *   in a call             hang up;
 *   looked at, silently   tell the indoor station the visitor rings (RING 1);
 *                         without an answer (LOOK 1) in ring_ms: RING 0.
 * Who is rung: the indoor stations that looked at or answered this one
 * (at most XIAOTAI_APP_DOORBELL_PEERS_MAX, remembered across restarts in
 * key space "xt_doorbell", key "peers"); while there is none, the first
 * XIAOTAI_APP_DOORBELL_PEERS_MAX devices among the contacts that are online,
 * in the list's order. So a device with
 * no screen is paired from the indoor station: add it there and look at the
 * door once.
 * A call from a device among the contacts is answered at once as a look; a
 * call from anyone else is rejected. Only the indoor station that answered
 * or looks may open the door (OPEN): with open_on_word the door counts as
 * opened (no lock is driven; a demonstration) and the cue sounds, otherwise
 * the answer is XIAOTAI_ERR_NOT_SUPPORTED. A remembered indoor station that
 * is no longer a contact is not rung and cannot look.
 *
 * Sounds (the "guide" and "cue" sounds of xiaotai/sounds.h): "正在呼叫"
 * (guide.dialing) when the ring begins, the ring-back while it rings,
 * cue.success when somebody answers and when the door opens, cue.warning when
 * nobody answered, cue.ended when a call ends. Lights: ringing, talking,
 * looked at. Alone in a product it speaks the binding code
 * (xiaotai/modules/binding.h) when it appears and every 30 s while it is
 * shown; beside other applications the setup is theirs.
 *
 * A mode beside other applications (a product that lists "doorbell" before
 * its home application, e.g. a small speaker that can also be the doorbell):
 * the doorbell acts only while it is the foreground application. It comes
 * forward with XIAOTAI_INTENT_APP_DOORBELL_OPEN (another application's menu
 * entry), a long press of the key goes back to the product's home
 * application (not while it rings or talks), and it is in front again after
 * a restart when it was opened and not left (key "front"; another
 * application brought forward to present an event does not count as
 * leaving). In front it presents every
 * device call event (an incoming call from a contact is answered as a look,
 * from anybody else turned away); otherwise the next application in the
 * product's order presents them. Leaving the front stops
 * a round. On a display it shows one call page: what happens now and what
 * the key does. Alone in a product it is always in front.
 *
 * Tags as in xiaotai/framework.h: [glue], [owner], [any]. */
#include <stdbool.h>
#include <stdint.h>
#include "xiaotai/framework.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t ring_ms;       /* each indoor station rings this long; 0: XIAOTAI_APP_DOORBELL_RING_MS */
    bool open_on_word;      /* OPEN from the answering indoor station counts as opened (no lock) */
} xiaotai_app_doorbell_config_t;

#define XIAOTAI_APP_DOORBELL_RING_MS 30000U
#define XIAOTAI_APP_DOORBELL_PEERS_MAX 4U

/* [glue] Before xiaotai_framework_start(); copied. NULL: defaults.
 * XIAOTAI_ERR_INVALID_STATE after init. */
xiaotai_err_t xiaotai_app_doorbell_configure(const xiaotai_app_doorbell_config_t *config);

/* The key's intent (args.value unused). The console may post it. */
#define XIAOTAI_INTENT_APP_DOORBELL_PRESS "doorbell.press"
/* Bring the doorbell to the front; leave it for the product's home application. */
#define XIAOTAI_INTENT_APP_DOORBELL_OPEN "doorbell.open"
#define XIAOTAI_INTENT_APP_DOORBELL_LEAVE "doorbell.leave"

/* Call message types; the values of xiaotai/apps/door.h (XIAOTAI_APP_DOOR_MESSAGE_*). */
#define XIAOTAI_APP_DOORBELL_MESSAGE_OPEN 1U
#define XIAOTAI_APP_DOORBELL_MESSAGE_OPEN_RESULT 2U
#define XIAOTAI_APP_DOORBELL_MESSAGE_LOOK 3U
#define XIAOTAI_APP_DOORBELL_MESSAGE_RING 4U
/* Sent when a call the doorbell placed is answered, 1 byte XIAOTAI_APP_DOORBELL_MODE_BELL (the door
 * application's mode of a doorbell): an indoor station that has not chosen this doorbell adds it. */
#define XIAOTAI_APP_DOORBELL_MESSAGE_HELLO 5U
#define XIAOTAI_APP_DOORBELL_MODE_BELL 1U

typedef enum {
    XIAOTAI_APP_DOORBELL_IDLE = 0,
    XIAOTAI_APP_DOORBELL_RINGING,   /* the indoor stations are rung in turn */
    XIAOTAI_APP_DOORBELL_TALKING,   /* an indoor station answered, or talks while it looks */
    XIAOTAI_APP_DOORBELL_LOOKED,    /* an indoor station looks; the visitor is not heard inside */
} xiaotai_app_doorbell_state_t;

typedef struct {
    xiaotai_app_doorbell_state_t state;
    uint8_t peers;                  /* remembered indoor stations */
    uint8_t rung;                   /* indoor stations rung this time so far */
    uint32_t presses, rings, answered, unanswered, looks, rejected, opens, refused_opens;
} xiaotai_app_doorbell_status_t;

/* [owner] */
void xiaotai_app_doorbell_status(xiaotai_app_doorbell_status_t *status);

/* Lifecycle, called by the framework (generated application table). */
xiaotai_err_t xiaotai_app_doorbell_init(void);
void xiaotai_app_doorbell_start(void);
void xiaotai_app_doorbell_foreground(bool foreground);
bool xiaotai_app_doorbell_claim(const xiaotai_event_t *event);
/* Host tests only: back to the state before init. */
void xiaotai_app_doorbell_reset(void);

#ifdef __cplusplus
}
#endif
