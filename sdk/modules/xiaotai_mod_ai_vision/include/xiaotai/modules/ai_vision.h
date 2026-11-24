// SPDX-License-Identifier: MIT
#pragma once
/* Base module "ai_vision": the AI sees what the camera sees during a
 * conversation.
 *
 * The module is a session extension of the AI talk module's activity "ai"
 * (an attachment: begin, end, session and contributions in
 * xiaotai/framework.h). It contributes the video part of the conversation's
 * link for scene XIAOTAI_AI_TALK_SCENE (XIAOTAI_LINK_SLOT_VIDEO: sending on
 * video stream 0, JPEG, peer subscriptions to other streams refused, nothing
 * received); the AI talk module has no video API. While the activity's
 * generation is a link (the AI talk module sets it once the service accepted
 * the session and clears it when the conversation starts playing out its
 * tail) and vision is enabled, it sends still pictures from the camera on
 * that link (about one a second); it stops the moment the generation returns
 * to 0 or the activity ends. The pictures are encoded and paced by the video
 * engine (xiaotai/video.h); this module owns only when they flow.
 *
 * A camera or encoder failure never ends the conversation: vision reports
 * the failure and the conversation continues with audio alone.
 *
 * Tags as in xiaotai/framework.h: [init], [owner], [any]. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Product configuration. A zeroed struct is valid. */
typedef struct {
    /* true: the user's switch is a lasting setting "ai_vision.enabled"
     * (stored under "ai_vision") that applications may offer. Default false:
     * no setting is registered and nothing is stored; the switch starts on at
     * every boot and xiaotai_ai_vision_set_enabled / "vision.enable" change
     * it until the next one. */
    bool user_switch;
} xiaotai_ai_vision_config_t;

/* [glue] Before xiaotai_framework_start(), by the product glue; copied. NULL
 * or never called: all defaults. XIAOTAI_ERR_INVALID_STATE after the module's
 * init. */
xiaotai_err_t xiaotai_ai_vision_configure(const xiaotai_ai_vision_config_t *config);

typedef enum {
    XIAOTAI_AI_VISION_OFF = 0,     /* no conversation, vision disabled, or its tail is playing */
    XIAOTAI_AI_VISION_STARTING,    /* the camera is being opened */
    XIAOTAI_AI_VISION_RUNNING,     /* pictures are sent */
    XIAOTAI_AI_VISION_FAILED,      /* the camera or the encoder failed for this conversation; see error */
    XIAOTAI_AI_VISION_UNAVAILABLE, /* this device cannot produce the pictures (no camera, no JPEG encoder) */
} xiaotai_ai_vision_state_t;

typedef struct {
    bool enabled;                  /* the user's switch */
    xiaotai_ai_vision_state_t state;
    uint32_t link;                 /* generation of the link and video session in use; 0: none */
    xiaotai_err_t error;           /* why FAILED; XIAOTAI_OK otherwise */
    uint32_t sessions;             /* conversations vision took part in, since boot */
    uint32_t failures;
} xiaotai_ai_vision_status_t;

/* [owner] */
void xiaotai_ai_vision_status(xiaotai_ai_vision_status_t *status);

/* [owner] The user's switch (default on). With user_switch it is the setting
 * "ai_vision.enabled" and is stored; without, it lasts until the next boot.
 * Takes effect at once, also in a running conversation: off releases the
 * camera, on opens it. XIAOTAI_ERR_NOT_SUPPORTED in state UNAVAILABLE: the
 * switch still changes (and with user_switch is stored). */
xiaotai_err_t xiaotai_ai_vision_set_enabled(bool enabled);

/* "ai_vision.state": the state changed. args.value: the new
 * xiaotai_ai_vision_state_t; data: a xiaotai_ai_vision_status_t. */
#define XIAOTAI_EVENT_AI_VISION_STATE "ai_vision.state"

/* Intent "vision.enable": args.value 1 on, 0 off, -1 toggle; as
 * xiaotai_ai_vision_set_enabled. */
#define XIAOTAI_INTENT_VISION_ENABLE "vision.enable"

/* Setting "ai_vision.enabled" (bool, default 1, storage key "ai_vision"),
 * registered only with user_switch. Diagnostics section "ai_vision".
 * Event-log source "ai_vision". The video engine's own command (AT+VIDEO)
 * shows the uplink's counters. */

/* Module lifecycle (called by the framework through the generated tables).
 * init: reads the video engine's capabilities, declares the still-picture
 * profile to it, attaches to the activity "ai" with its link video part,
 * subscribes to the video engine's events, registers the setting (with
 * user_switch) and the intent. A device that cannot produce the pictures
 * initializes successfully in state UNAVAILABLE and contributes nothing. */
xiaotai_err_t xiaotai_module_ai_vision_init(void);
/* Host tests: forget state. */
void xiaotai_ai_vision_reset(void);

#ifdef __cplusplus
}
#endif
