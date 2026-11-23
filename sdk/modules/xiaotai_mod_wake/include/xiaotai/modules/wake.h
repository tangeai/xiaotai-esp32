// SPDX-License-Identifier: MIT
#pragma once
/* Base module "wake": the wake word starts a conversation.
 *
 * The wake-word engine (xiaotai/wake.h) scores audio and reports detections.
 * This module decides when the device listens for the phrase and what an
 * accepted detection leads to:
 *   - it listens only while the user enabled it, no foreground activity is
 *     current and the system is not quiesced (the engine and the media engine
 *     add their own conditions: microphone not muted, nothing recording);
 *   - on a detection it holds the pre-roll (the audio captured since the
 *     phrase), starts an AI conversation with it and tells the application;
 *     a loss of the held pre-roll before the conversation binds it is
 *     forwarded to xiaotai_ai_talk_preroll_lost, which ends that
 *     conversation.
 * It plays no sound and shows nothing: the acknowledgement is the
 * application's, driven by XIAOTAI_EVENT_WAKE_DETECTED.
 *
 * Names. The manifest name is "wake" and the module's event, intent,
 * setting and event-log source are "wake.*" / "wake". Its functions are
 * xiaotai_local_wake_* because the engine (manifest name "wake_engine")
 * already owns xiaotai_wake_*; the engine's tuning keys and diagnostics are
 * its own (xiaotai/wake.h).
 *
 * Tags as in xiaotai/framework.h: [init], [owner], [any]. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------- configuration */

/* Product configuration. 0 (false) selects the stated default. */
typedef struct {
    /* Audio from before the end of the phrase that the conversation starts
     * with. Default 800. */
    uint32_t keep_before_ms;
    /* How long the held pre-roll waits for its conversation. Default 19000. */
    uint32_t preroll_ttl_ms;
    /* Pre-roll capacity declared to the media engine. Default 20000. */
    uint32_t preroll_ms;
    /* true: a detection only emits the event; nothing is held and no
     * conversation is started. Default false. */
    bool announce_only;
    /* true: the user's switch is a lasting setting "wake.enabled" (stored
     * under "wake_on") that applications may offer. Default false: no setting
     * is registered and nothing is stored; the switch starts on at every
     * boot and xiaotai_local_wake_set_enabled / "wake.enable" change it until
     * the next one. */
    bool user_switch;
} xiaotai_local_wake_config_t;

/* [glue] Before xiaotai_framework_start(), by the product glue; copied. NULL
 * or never called: all defaults. XIAOTAI_ERR_INVALID_STATE after the module's
 * init. */
xiaotai_err_t xiaotai_local_wake_configure(const xiaotai_local_wake_config_t *config);

/* ------------------------------------------------------------------ control */

/* [owner] The user's switch (default on). With user_switch it is the setting
 * "wake.enabled" and is stored; without, it lasts until the next boot.
 * XIAOTAI_ERR_NOT_SUPPORTED when the engine has no model or the board no PCM
 * capture: the switch still changes (and with user_switch is stored). */
xiaotai_err_t xiaotai_local_wake_set_enabled(bool enabled);

/* What a detection led to. Appended, never renumbered. */
typedef enum {
    XIAOTAI_LOCAL_WAKE_STARTED = 0,    /* a conversation was started */
    XIAOTAI_LOCAL_WAKE_ANNOUNCED,      /* announce_only: reported, nothing started */
    XIAOTAI_LOCAL_WAKE_BUSY,           /* an activity began in the meantime, or the system is quiesced */
    XIAOTAI_LOCAL_WAKE_NO_PREROLL,     /* the media engine could not hold the pre-roll */
    XIAOTAI_LOCAL_WAKE_REFUSED,        /* the AI talk module refused to start; see error */
} xiaotai_local_wake_outcome_t;

/* Payload of XIAOTAI_EVENT_WAKE_DETECTED. */
typedef struct {
    uint32_t sequence;         /* the engine's detection count; 0 for an injected phrase */
    uint16_t score_milli;      /* 0 for an injected phrase */
    uint16_t threshold_milli;
    xiaotai_local_wake_outcome_t outcome;
    xiaotai_err_t error;       /* XIAOTAI_OK unless NO_PREROLL or REFUSED */
    uint32_t session;          /* the conversation it started (xiaotai/modules/ai_talk.h); 0: none */
} xiaotai_local_wake_detection_t;

/* Why the device does not listen now. */
typedef enum {
    XIAOTAI_LOCAL_WAKE_LISTENING = 0,
    XIAOTAI_LOCAL_WAKE_OFF_DISABLED,     /* the user's switch */
    XIAOTAI_LOCAL_WAKE_OFF_ACTIVITY,     /* a foreground activity is current */
    XIAOTAI_LOCAL_WAKE_OFF_QUIESCED,
    XIAOTAI_LOCAL_WAKE_OFF_UNAVAILABLE,  /* no model, or no PCM capture */
} xiaotai_local_wake_gate_t;

typedef struct {
    bool enabled;                       /* the user's switch */
    xiaotai_local_wake_gate_t gate;     /* what was last asked of the engine and why */
    uint32_t detections;                /* reported by the engine, since boot */
    uint32_t started, announced, busy, no_preroll, refused;
    uint32_t injected;                  /* phrases injected by the development command */
    xiaotai_local_wake_detection_t last;/* zeroed before the first detection */
} xiaotai_local_wake_status_t;

/* [owner] */
void xiaotai_local_wake_status(xiaotai_local_wake_status_t *status);

/* -------------------------------------------------- events and intents */

/* "wake.detected": the engine accepted a detection (or a phrase was
 * injected) and the module acted on it. args.value: the
 * xiaotai_local_wake_outcome_t; data: a xiaotai_local_wake_detection_t.
 * Emitted after the conversation was started, so a handler that reads the AI
 * talk status sees it CONNECTING. */
#define XIAOTAI_EVENT_WAKE_DETECTED "wake.detected"

/* Intent "wake.enable": args.value 1 on, 0 off, -1 toggle; as
 * xiaotai_local_wake_set_enabled. */
#define XIAOTAI_INTENT_WAKE_ENABLE "wake.enable"

/* Setting "wake.enabled" (bool, default 1, storage key "wake_on"), registered
 * only with user_switch.
 * Commands: the verbs of AT+WAKE that interpret text against the model's
 * phrases, so phrase matching lives here only (the engine registers the
 * status, THRESHOLD and WATCH, xiaotai/wake.h). A text matches when, after
 * leading ASCII white space, it begins with one of the model's phrases.
 *   AT+WAKE TEST <phrase-hex>  takes the path of a detection without
 *       pre-roll when the text matches; reply +WAKE-TEST: intent=wake
 *       outcome=<xiaotai_local_wake_outcome_t>.
 *   AT+WAKE SELFTEST  checks the matcher, starts nothing: every phrase alone
 *       and every phrase after a space and followed by a continuation must
 *       match, and one text without a phrase must not; reply
 *       +WAKE-SELFTEST: passed=<n> total=<n> (two cases per phrase plus one),
 *       then OK when all passed, else ERROR. NOT_SUPPORTED when the model
 *       lists no phrases or there is no model.
 * Diagnostics section "wake" (the engine's is "wake_engine"). The provider
 * writes its line with the prefix +WAKE-MODULE: itself, because the console's
 * default prefix for the section, +WAKE:, is the line the engine's AT+WAKE
 * status and WATCH already print; tools that count +WAKE: groups must not see
 * a second record under it. Event-log source "wake". */

/* ---------------------------------------------------------------- lifecycle */

/* Module lifecycle (called by the framework through the generated tables).
 * init: declares the pre-roll to the media engine, subscribes to the engine's
 * detections and to the framework's activity and quiesce events, registers
 * the setting (with user_switch), the intent and the command verbs. start:
 * applies the gate. */
xiaotai_err_t xiaotai_module_wake_init(void);
void xiaotai_module_wake_start(void);
/* Host tests: forget configuration and state. */
void xiaotai_local_wake_reset(void);

#ifdef __cplusplus
}
#endif
