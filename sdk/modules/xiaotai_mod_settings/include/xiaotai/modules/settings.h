// SPDX-License-Identifier: MIT
#pragma once
/* Settings module: the settings every product shares.
 *
 * Speaker volume, speaker mute, microphone mute, microphone gain, voice
 * guidance and the screen's sleep time are the same on every product; this
 * module registers them with the framework's settings registry
 * (xiaotai/framework.h), so they persist, appear in settings screens and are
 * reachable through AT+SET, and it applies the audio ones to the media engine
 * (which holds no setting and stores nothing).
 *
 * Who applies what:
 *   audio.*            this module, to the media engine, at start and on
 *                      every change; a value the engine refuses at once is
 *                      taken back (the setting returns to the last applied
 *                      value); when its task fails later
 *                      (XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR) the setting returns
 *                      to the applied value in xiaotai_media_status in builds
 *                      with XIAOTAI_SETTINGS_TAKE_BACK_LATE_REFUSAL
 *                      (xiaotai/modules/settings_config.h) and keeps the
 *                      user's value otherwise; a failure of the values
 *                      applied at start is never taken back; a volume
 *                      failure during a preview only ends the preview
 *   guidance.enabled,  not applied here (this module depends on neither the
 *   screen.sleep       feedback engine nor the view system): the application
 *                      reads them at start and on
 *                      XIAOTAI_EVENT_SETTINGS_CHANGED and calls
 *                      xiaotai_feedback_group_enable / xiaotai_ui_set_sleep_timeout
 *
 * Where values are stored is decided by the preferences service
 * (xiaotai/preferences.h): a product whose deployed devices hold these
 * preferences in another place or shape lists them in its binding table under
 * the storage keys below, and the device keeps what its user chose. The
 * setting's value is always in the unit named here, whatever the stored
 * number is (a 0..10 level, an index into a table of minutes).
 *
 * Settings of one module or application (the answer voice, the idle
 * expression, a door station's peer) are registered by their owner, not here.
 *
 * Manifest name "settings". Depends on the engines identity (preferences),
 * eventlog and at; optionally on the media engine (used when the product
 * selects it, never pulled in). No hardware need: a setting whose hardware is
 * absent is not registered, and without the media engine no audio setting
 * (audio.*, guidance.enabled) is registered at all.
 *
 * Task tags as in xiaotai/framework.h. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ settings */

/* Names (for xiaotai_setting_get_int / _set_int and AT+SET) and storage keys
 * (for a product's preference binding table). Kind, unit, range, default.
 * "Registered when" names the hardware the setting needs; every audio.* and
 * guidance.enabled setting also needs the media engine in the build. */

/* Speaker volume. Int, percent 0..100; default: configuration (70).
 * Registered when the board has an audio output. */
#define XIAOTAI_SETTING_AUDIO_VOLUME "audio.volume"
#define XIAOTAI_SETTING_KEY_AUDIO_VOLUME "volume"
/* Speaker mute. Bool; default 0. Registered when the board has an audio output. */
#define XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED "audio.speaker_muted"
#define XIAOTAI_SETTING_KEY_AUDIO_SPEAKER_MUTED "spk_mute"
/* Microphone mute, the user's lasting choice. Bool; default 0. Registered when
 * the board has an audio input.
 * A call module's mute during a call (xiaotai_media_set_mic_muted) lies on
 * top of it: the microphone is muted while either says so. Rules: this module
 * applies every change of the setting at once, during a call too, and the new
 * value is then also the call's mute state (one microphone, one switch the user
 * sees); a call module that ends its own mute, or ends the call, applies the
 * setting's current value (0 when the setting is not registered), never the
 * value it saw when the call began. So a change made during a call outlives
 * the call, and a mute made only for the call does not. */
#define XIAOTAI_SETTING_AUDIO_MIC_MUTED "audio.mic_muted"
#define XIAOTAI_SETTING_KEY_AUDIO_MIC_MUTED "mic_mute"
/* Microphone gain. Int, dB; range and default are the first audio input
 * part's gain_min_db..gain_max_db and gain_db. Registered when that part's
 * gain is adjustable (gain_min_db < gain_max_db). */
#define XIAOTAI_SETTING_AUDIO_MIC_GAIN "audio.mic_gain"
#define XIAOTAI_SETTING_KEY_AUDIO_MIC_GAIN "mic_gain"
/* Automatic voice guidance (spoken state changes and the automatic spoken
 * verification code; explicit replays and warnings are not affected). Bool;
 * default 1. Registered when the board has an audio output. Applied by the
 * application only (its feedback groups, its binding announcements); no
 * module reads it. */
#define XIAOTAI_SETTING_GUIDANCE_ENABLED "guidance.enabled"
#define XIAOTAI_SETTING_KEY_GUIDANCE_ENABLED "guidance"
/* Screen idle time before it sleeps. Int, seconds 0..86400, 0: never;
 * default: configuration (300). Registered when the board has a display. */
#define XIAOTAI_SETTING_SCREEN_SLEEP "screen.sleep"
#define XIAOTAI_SETTING_KEY_SCREEN_SLEEP "sleep"

/* [owner] Confirm a deliberate text selection from a view. A different
 * value uses the normal setter; confirming the current value saves it
 * without a change event. The usual validation and maintenance gate apply.
 * Not for level synchronization or AT+SET, which remain idempotent. */
xiaotai_err_t xiaotai_settings_confirm_text(const char *name, const char *value);
/* [owner] Save a deliberately reselected integer without changing it. If
 * the value has since changed, refuse rather than restoring the old choice. */
xiaotai_err_t xiaotai_settings_confirm_int_selection(const char *name, int32_t value);

/* -------------------------------------------------------------------- status */

typedef struct {
    uint32_t revision;           /* increases whenever any field below changes */
    /* Which settings exist on this board. */
    bool has_speaker, has_microphone, has_mic_gain, has_screen;
    /* Values of the settings (0 / false for those that do not exist). */
    uint8_t volume;
    bool speaker_muted;
    bool mic_muted;
    int8_t mic_gain_db, mic_gain_min_db, mic_gain_max_db;
    bool guidance;
    uint32_t screen_sleep_s;
    /* A volume preview is active: the speaker plays at preview_volume while
     * the setting still holds volume. */
    bool previewing;
    uint8_t preview_volume;
    /* Result of the last attempt to apply an audio setting; XIAOTAI_OK when
     * it was accepted. */
    xiaotai_err_t apply_error;
    /* Optional diagnostic for an unreadable packed settings record. */
    xiaotai_err_t packed_load_error;
    /* Accepted changes have not all reached storage yet
     * (xiaotai_preferences_pending()). */
    bool save_pending;
} xiaotai_settings_status_t;

/* [any] Snapshot. */
void xiaotai_settings_status(xiaotai_settings_status_t *status);

/* --------------------------------------------------------------- convenience */

/* The functions below are shortcuts for inputs (keys, a slider, a voice
 * command). Everything else goes through the registry: xiaotai_setting_set_int. */

/* [owner] Move the volume by steps times the configured step, clamped to
 * 0..100. At a limit nothing changes and the result is still XIAOTAI_OK.
 * With XIAOTAI_SETTINGS_VOLUME_UP_UNMUTES (xiaotai/modules/settings_config.h)
 * raising the volume also clears the speaker mute.
 * XIAOTAI_ERR_BUSY while the system is quiesced (as every setting change).
 * XIAOTAI_ERR_NOT_SUPPORTED without an audio output or without the media
 * engine in the build (also for the preview); the media engine's
 * result when it refuses the new value (the setting then keeps its value). */
xiaotai_err_t xiaotai_settings_volume_step(int steps);

/* [owner] Effective product step for controls that display a volume slider. */
uint8_t xiaotai_settings_volume_step_percent(void);

/* [owner] Explicit user preference, distinct from a temporary session mute.
 * Requests the media gate before writing the setting. On a media refusal or
 * while quiesced, keeps the previous preference. Once accepted, persistence
 * uses the normal setting store and its SETTING_NOT_SAVED notification.
 * NOT_SUPPORTED when the microphone setting is unavailable. */
xiaotai_err_t xiaotai_settings_set_mic_mute(bool muted);

/* [owner] Play at percent without changing the setting: for a slider that is
 * being dragged. The preview ends with the next change of the volume setting
 * (the slider's release stores the final value) or with
 * xiaotai_settings_volume_restore(). A release at the setting's own value is
 * no change, so a slider calls xiaotai_settings_volume_restore() after
 * storing (it does nothing once the preview has ended, and never sends the
 * engine a value it already plays). XIAOTAI_ERR_INVALID_ARG above 100;
 * XIAOTAI_ERR_BUSY while the system is quiesced (settings cannot change
 * during maintenance, and neither does what is heard). */
xiaotai_err_t xiaotai_settings_volume_preview(uint8_t percent);

/* [owner] End a preview and return to the setting's value. No preview: nothing. */
void xiaotai_settings_volume_restore(void);

/* ------------------------------------------------------------- configuration */

/* Product configuration. */
typedef struct {
    uint8_t volume_default;      /* percent; 0 selects 70 */
    uint8_t volume_step;         /* percent per step; 0 selects 10 */
    /* Default screen idle time, seconds; 0 selects 300; negative: never. */
    int32_t screen_sleep_default_s;
    /* Default of guidance.enabled is on; true turns it off by default. */
    bool guidance_default_off;
    /* Require synchronous media acceptance before saving a speaker toggle. */
    bool speaker_accept_before_save;
    /* Require synchronous media acceptance before saving a volume change. */
    bool volume_accept_before_save;
} xiaotai_settings_config_t;

/* [glue] Before xiaotai_module_settings_init(), from the product glue; copied. NULL
 * restores the defaults. XIAOTAI_ERR_INVALID_ARG: a volume above 100, a step
 * above 50, a sleep time above 86400. XIAOTAI_ERR_INVALID_STATE once
 * initialized. */
xiaotai_err_t xiaotai_settings_configure(const xiaotai_settings_config_t *config);

/* -------------------------------------------------------------------- events */

/* One of this module's settings changed (after it was applied, for the audio
 * ones). args.text: the setting's name; args.value: its new value. Also
 * emitted once per registered setting right after XIAOTAI_EVENT_STARTED, so a
 * subscriber applies the stored values without a second code path. */
#define XIAOTAI_EVENT_SETTINGS_CHANGED "settings.changed"
/* An audio setting was refused by the hardware and taken back, at once or on
 * XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR (its text "volume", "mute" or "gain" names
 * the setting; "power" takes nothing back; a late refusal is taken back only
 * with XIAOTAI_SETTINGS_TAKE_BACK_LATE_REFUSAL, see above). args.text: the
 * setting's name; args.value: the xiaotai_err_t. */
#define XIAOTAI_EVENT_SETTINGS_REFUSED "settings.refused"

/* ------------------------------------------------------------------- intents */

/* xiaotai_settings_volume_step(+1) / (-1). No arguments. */
#define XIAOTAI_INTENT_AUDIO_VOLUME_UP "audio.volume_up"
#define XIAOTAI_INTENT_AUDIO_VOLUME_DOWN "audio.volume_down"
/* Set the volume to args.value percent. */
#define XIAOTAI_INTENT_AUDIO_VOLUME_SET "audio.volume_set"
/* Speaker and microphone mute. args.value: 1 mute, 0 unmute, -1 toggle. */
#define XIAOTAI_INTENT_AUDIO_SPEAKER_MUTE "audio.speaker_mute"
#define XIAOTAI_INTENT_AUDIO_MIC_MUTE "audio.mic_mute"

/* -------------------------------------------------------------- diagnostics */

typedef struct {
    uint32_t changes;            /* setting changes handled */
    uint32_t applied;            /* values handed to the media engine */
    uint32_t refused;            /* of which refused and taken back */
    uint32_t steps, steps_at_limit;
    uint32_t previews;
    uint32_t not_saved;          /* XIAOTAI_EVENT_SETTING_NOT_SAVED seen for one of these settings */
} xiaotai_settings_stats_t;

/* [any] */
void xiaotai_settings_stats(xiaotai_settings_stats_t *stats);

/* ----------------------------------------------------------------- lifecycle */

/* Called by the framework through the generated tables. init: registers the
 * settings the board's parts allow (which loads their stored values), the
 * intents and the diagnostics provider "settings", and with the media engine
 * subscribes to XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR and stages the stored values
 * before the media engine starts. The initial XIAOTAI_EVENT_SETTINGS_CHANGED
 * events are emitted while XIAOTAI_EVENT_STARTED is dispatched, so every
 * subscriber hears them after XIAOTAI_EVENT_STARTED. */
xiaotai_err_t xiaotai_module_settings_init(void);

/* Host tests: forget configuration, preview and counters. */
void xiaotai_settings_reset(void);

#ifdef __cplusplus
}
#endif
