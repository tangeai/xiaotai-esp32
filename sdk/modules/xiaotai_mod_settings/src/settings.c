// SPDX-License-Identifier: MIT
/* Settings module: configuration, the screen's sleep time, status, statistics
 * and the lifecycle. The audio settings are in with_media.c or audio_none.c.
 *
 * The module has no task and no timer: the registry's changed() callbacks,
 * the intents and every event it handles run on the owner task. It allocates
 * nothing; the registry keeps the values, the setting store persists them. */
#include <string.h>
#include "settings_internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/hal.h"
#include "xiaotai/preferences.h"

#define VOLUME_DEFAULT 70U
#define VOLUME_STEP_DEFAULT 10U
#define VOLUME_STEP_MAX 50U
#define SLEEP_DEFAULT_S 300
#define SLEEP_MAX_S 86400

settings_state_t xiaotai_settings_state;
#define S xiaotai_settings_state

/* ------------------------------------------------------------ configuration */

xiaotai_err_t xiaotai_settings_configure(const xiaotai_settings_config_t *config)
{
    if (S.initialized) return XIAOTAI_ERR_INVALID_STATE;
    if (!config) {
        memset(&S.config, 0, sizeof(S.config));
        return XIAOTAI_OK;
    }
    if (config->volume_default > 100U || config->volume_step > VOLUME_STEP_MAX ||
        config->screen_sleep_default_s > SLEEP_MAX_S) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    S.config = *config;
    return XIAOTAI_OK;
}

uint8_t xiaotai_settings_volume_default(void)
{
    return S.config.volume_default ? S.config.volume_default : (uint8_t)VOLUME_DEFAULT;
}

uint8_t xiaotai_settings_volume_step_percent(void)
{
    return S.config.volume_step ? S.config.volume_step : (uint8_t)VOLUME_STEP_DEFAULT;
}

bool xiaotai_settings_guidance_default(void) { return !S.config.guidance_default_off; }

static int32_t sleep_default(void)
{
    if (S.config.screen_sleep_default_s < 0) return 0;
    return S.config.screen_sleep_default_s ? S.config.screen_sleep_default_s : SLEEP_DEFAULT_S;
}

/* ------------------------------------------------------------------ helpers */

void xiaotai_settings_touch(void) { atomic_fetch_add(&S.revision, 1U); }

void xiaotai_settings_registered(const char *name)
{
    if (S.count < SETTINGS_MAX) S.names[S.count++] = name;
}

void xiaotai_settings_emit(const char *event, const char *name, int32_t value)
{
    xiaotai_args_t args = {.value = value};
    strncpy(args.text, name, sizeof(args.text) - 1);
    /* A full owner queue loses the event; the registry still holds the value,
     * and the next change or a status read brings the subscriber up to date. */
    if (xiaotai_event_emit(event, &args, NULL, 0) != XIAOTAI_OK) {
        xiaotai_eventlog_warn(SETTINGS_SOURCE, "event.lost", 0, value);
    }
}

static bool ours(const char *name)
{
    for (size_t i = 0; i < S.count; ++i) {
        if (strcmp(S.names[i], name) == 0) return true;
    }
    return false;
}

/* ------------------------------------------------------------------- screen */

/* [owner] Applied by the application (the view system is not a dependency). */
static void screen_changed(const char *name, void *ctx)
{
    (void)ctx;
    int32_t value = 0;
    if (xiaotai_setting_get_int(name, &value) != XIAOTAI_OK) return;
    atomic_fetch_add(&S.counters.changes, 1U);
    xiaotai_settings_touch();
    xiaotai_settings_emit(XIAOTAI_EVENT_SETTINGS_CHANGED, name, value);
}

static xiaotai_err_t register_screen(void)
{
    if (!xiaotai_hal_display(0)) return XIAOTAI_OK;
    const xiaotai_setting_t setting = {
        .name = XIAOTAI_SETTING_SCREEN_SLEEP, .key = XIAOTAI_SETTING_KEY_SCREEN_SLEEP,
        .kind = XIAOTAI_SETTING_INT, .min = 0, .max = SLEEP_MAX_S, .default_value = sleep_default(),
        .changed = screen_changed,
    };
    const xiaotai_err_t err = xiaotai_setting_register(&setting);
    if (err != XIAOTAI_OK) return err;
    atomic_store(&S.has_screen, true);
    xiaotai_settings_registered(XIAOTAI_SETTING_SCREEN_SLEEP);
    return XIAOTAI_OK;
}

/* ------------------------------------------------------------------- events */

/* [owner] The stored values reach the application through the same event as
 * every later change: once per registered setting, after system.started. */
static void on_started(const xiaotai_event_t *event, void *ctx)
{
    (void)event;
    (void)ctx;
    for (size_t i = 0; i < S.count; ++i) {
        int32_t value = 0;
        if (xiaotai_setting_get_int(S.names[i], &value) == XIAOTAI_OK) {
            xiaotai_settings_emit(XIAOTAI_EVENT_SETTINGS_CHANGED, S.names[i], value);
        }
    }
}

static void on_not_saved(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (!ours(event->args.text)) return;
    atomic_fetch_add(&S.counters.not_saved, 1U);
    xiaotai_eventlog_error(SETTINGS_SOURCE, "not_saved", 0, (xiaotai_err_t)event->args.value);
}

/* ------------------------------------------------------------------- status */

static uint8_t get_u8(const char *name)
{
    int32_t value = 0;
    return xiaotai_setting_get_int(name, &value) == XIAOTAI_OK ? (uint8_t)value : 0U;
}

static int32_t get_i32(const char *name)
{
    int32_t value = 0;
    return xiaotai_setting_get_int(name, &value) == XIAOTAI_OK ? value : 0;
}

void xiaotai_settings_status(xiaotai_settings_status_t *status)
{
    if (!status) return;
    memset(status, 0, sizeof(*status));
    status->has_speaker = atomic_load(&S.has_speaker);
    status->has_microphone = atomic_load(&S.has_microphone);
    status->has_mic_gain = atomic_load(&S.has_mic_gain);
    status->has_screen = atomic_load(&S.has_screen);
    if (status->has_speaker) {
        status->volume = get_u8(XIAOTAI_SETTING_AUDIO_VOLUME);
        status->speaker_muted = get_i32(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED) != 0;
    }
    if (status->has_microphone) status->mic_muted = get_i32(XIAOTAI_SETTING_AUDIO_MIC_MUTED) != 0;
    if (status->has_mic_gain) {
        status->mic_gain_db = (int8_t)get_i32(XIAOTAI_SETTING_AUDIO_MIC_GAIN);
        status->mic_gain_min_db = S.gain_min_db;
        status->mic_gain_max_db = S.gain_max_db;
    }
    if (atomic_load(&S.has_guidance)) status->guidance = get_i32(XIAOTAI_SETTING_GUIDANCE_ENABLED) != 0;
    if (status->has_screen) status->screen_sleep_s = (uint32_t)get_i32(XIAOTAI_SETTING_SCREEN_SLEEP);
    status->previewing = atomic_load(&S.previewing);
    status->preview_volume = status->previewing ? atomic_load(&S.preview_volume) : 0U;
    status->apply_error = atomic_load(&S.apply_error);
    status->packed_load_error = xiaotai_preferences_packed_load_error();
    /* Storage completes on its own worker and tells nobody: the revision
     * follows save_pending when a reader sees it change. */
    status->save_pending = xiaotai_preferences_pending();
    if (atomic_exchange(&S.save_pending_seen, status->save_pending) != status->save_pending) {
        xiaotai_settings_touch();
    }
    status->revision = atomic_load(&S.revision);
}

void xiaotai_settings_stats(xiaotai_settings_stats_t *stats)
{
    if (!stats) return;
    stats->changes = atomic_load(&S.counters.changes);
    stats->applied = atomic_load(&S.counters.applied);
    stats->refused = atomic_load(&S.counters.refused);
    stats->steps = atomic_load(&S.counters.steps);
    stats->steps_at_limit = atomic_load(&S.counters.steps_at_limit);
    stats->previews = atomic_load(&S.counters.previews);
    stats->not_saved = atomic_load(&S.counters.not_saved);
}

/* ---------------------------------------------------------------- lifecycle */

/* Runtime state back to "nothing registered"; the configuration stays. */
static void forget_runtime(void)
{
    S.initialized = false;
    memset(S.names, 0, sizeof(S.names));
    S.count = 0;
    atomic_store(&S.has_speaker, false);
    atomic_store(&S.has_microphone, false);
    atomic_store(&S.has_mic_gain, false);
    atomic_store(&S.has_screen, false);
    atomic_store(&S.has_guidance, false);
    S.gain_min_db = S.gain_max_db = 0;
    atomic_store(&S.previewing, false);
    atomic_store(&S.preview_volume, 0U);
    atomic_store(&S.apply_error, XIAOTAI_OK);
    atomic_store(&S.save_pending_seen, false);
    xiaotai_settings_audio_reset();
}

xiaotai_err_t xiaotai_module_settings_init(void)
{
    /* A start that failed after this init left nothing the framework still
     * holds (it forgets every registration), so start from scratch. */
    forget_runtime();
    xiaotai_err_t err = xiaotai_settings_audio_register();
    if (err == XIAOTAI_OK) err = register_screen();
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_STARTED, on_started, NULL);
    if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_SETTING_NOT_SAVED, on_not_saved, NULL);
    if (err == XIAOTAI_OK) err = xiaotai_diag_register(SETTINGS_SOURCE, xiaotai_settings_diag_report);
    if (err != XIAOTAI_OK) {
        xiaotai_eventlog_error(SETTINGS_SOURCE, "init.failed", 0, err);
        return err;
    }
    S.initialized = true;
    xiaotai_settings_audio_prepare();
    xiaotai_settings_touch();
    return XIAOTAI_OK;
}

void xiaotai_settings_reset(void)
{
    forget_runtime();
    memset(&S.config, 0, sizeof(S.config));
    atomic_store(&S.revision, 0U);
    settings_counters_t *c = &S.counters;
    atomic_store(&c->changes, 0U);
    atomic_store(&c->applied, 0U);
    atomic_store(&c->refused, 0U);
    atomic_store(&c->steps, 0U);
    atomic_store(&c->steps_at_limit, 0U);
    atomic_store(&c->previews, 0U);
    atomic_store(&c->not_saved, 0U);
    atomic_store(&c->output_errors, 0U);
    atomic_store(&c->late_kept, 0U);
    atomic_store(&c->take_back_deferred, 0U);
}
