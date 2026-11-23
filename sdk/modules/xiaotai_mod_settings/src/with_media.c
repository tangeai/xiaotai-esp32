// SPDX-License-Identifier: MIT
/* Audio settings with the media engine: registration by the board's parts,
 * applying every change, taking back what the hardware refuses, the volume
 * preview and steps, and the audio intents.
 *
 * Applying. The engine records volume, mute and gain requests and applies
 * them on its own task; it answers at once only for arguments and
 * capabilities, and reports a later failure with
 * XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR (its applied values stay at the last
 * success). Each audio setting remembers the last value the engine accepted.
 * A synchronous speaker or volume refusal can be rejected before persistence
 * by independent product options. Other values the engine refuses at once
 * are taken back; a late refusal
 * takes the setting back to the engine's applied value when the build asks
 * for it (XIAOTAI_SETTINGS_TAKE_BACK_LATE_REFUSAL) and only for a value the
 * user changed since start, so a failure while applying the stored values at
 * start never overwrites what the user chose. A take-back goes through the
 * registry (it persists and other subscribers see the value) with a re-entry
 * flag, so the take-back is not applied again.
 *
 * Preview. A slider being dragged plays at its position without touching
 * the setting. The next change of the volume setting (the release) ends it;
 * a release at the previewed value is not sent to the engine twice. */
#include <string.h>
#include "settings_internal.h"
#include "xiaotai/eventlog.h"
#include "xiaotai/hal.h"
#include "xiaotai/media.h"
#include "xiaotai/modules/settings_config.h"
#include "xiaotai/preferences.h"

#define S xiaotai_settings_state

typedef enum {
    AUDIO_VOLUME = 0,
    AUDIO_SPEAKER_MUTED,
    AUDIO_MIC_MUTED,
    AUDIO_MIC_GAIN,
    AUDIO_GUIDANCE,
    AUDIO_COUNT,
} audio_id_t;

static const char *const NAMES[AUDIO_COUNT] = {
    XIAOTAI_SETTING_AUDIO_VOLUME, XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED, XIAOTAI_SETTING_AUDIO_MIC_MUTED,
    XIAOTAI_SETTING_AUDIO_MIC_GAIN, XIAOTAI_SETTING_GUIDANCE_ENABLED,
};

/* Owner task only. */
static struct {
    bool registered[AUDIO_COUNT];
    int32_t applied[AUDIO_COUNT];   /* last value the engine accepted */
    uint32_t dirty;                 /* bit per setting: a user change reached the engine since start */
    uint32_t take_back_pending;     /* bit per setting: a take-back the registry refused while quiesced */
    bool taking_back;               /* the registry change in progress is a take-back */
    bool mic_preaccepted;           /* the explicit user action already submitted the media request */
    xiaotai_err_t last_result;      /* result of the last apply made from changed() */
    uint8_t ceiling;                /* the output's volume ceiling 1..99; 0: none */
    bool marked;                    /* the stored volume is known to be for that ceiling */
    uint8_t convert_from;           /* ceiling a stored volume was saved for (100: whole range); 0: nothing to convert */
} s_audio;

static uint32_t bit(audio_id_t id) { return 1U << (unsigned)id; }

/* ---------------------------------------------------------------- applying */

static xiaotai_err_t apply(audio_id_t id, int32_t value)
{
    xiaotai_err_t err = XIAOTAI_OK;
    switch (id) {
    case AUDIO_VOLUME: err = xiaotai_media_set_volume((uint8_t)value); break;
    case AUDIO_SPEAKER_MUTED: err = xiaotai_media_set_speaker_muted(value != 0); break;
    case AUDIO_MIC_MUTED: err = xiaotai_media_set_mic_muted(value != 0); break;
    case AUDIO_MIC_GAIN: err = xiaotai_media_set_input_gain((int8_t)value); break;
    default: return XIAOTAI_OK;
    }
    atomic_fetch_add(&S.counters.applied, 1U);
    atomic_store(&S.apply_error, err);
    xiaotai_settings_touch();
    return err;
}

static void end_preview(void)
{
    if (atomic_exchange(&S.previewing, false)) xiaotai_settings_touch();
}

static bool accepts_before_save(audio_id_t id)
{
    return (id == AUDIO_VOLUME && S.config.volume_accept_before_save) ||
           (id == AUDIO_SPEAKER_MUTED && S.config.speaker_accept_before_save);
}

static void restore_refused_preview(xiaotai_err_t refusal)
{
    const xiaotai_err_t restored = apply(AUDIO_VOLUME, s_audio.applied[AUDIO_VOLUME]);
    if (restored != XIAOTAI_OK) xiaotai_eventlog_warn(SETTINGS_SOURCE, "restore.refused", 0, restored);
    atomic_store(&S.apply_error, refusal);
}

static bool accept_audio_int(int32_t value, void *ctx)
{
    if (s_audio.taking_back) return true;
    const audio_id_t id = (audio_id_t)(uintptr_t)ctx;
    const bool previewing = id == AUDIO_VOLUME && atomic_load(&S.previewing);
    const uint8_t previewed = previewing ? atomic_load(&S.preview_volume) : 0;
    if (previewing && previewed == (uint8_t)value) return true;

    const xiaotai_err_t err = apply(id, value);
    s_audio.last_result = err;
    if (err == XIAOTAI_OK) return true;
    if (previewing) {
        end_preview();
        if (previewed != (uint8_t)s_audio.applied[id]) restore_refused_preview(err);
    }
    atomic_fetch_add(&S.counters.changes, 1U);
    atomic_fetch_add(&S.counters.refused, 1U);
    xiaotai_eventlog_error(SETTINGS_SOURCE, "refused", 0, err);
    xiaotai_settings_emit(XIAOTAI_EVENT_SETTINGS_REFUSED, NAMES[id], err);
    return false;
}

/* Returns the setting to value without applying it, then tells subscribers. */
static void take_back(audio_id_t id, int32_t value, xiaotai_err_t refusal)
{
    s_audio.taking_back = true;
    const xiaotai_err_t err = xiaotai_setting_set_int(NAMES[id], value);
    s_audio.taking_back = false;
    if (err == XIAOTAI_ERR_BUSY) {
        /* Quiesced: the registry accepts nothing now; done on release. */
        s_audio.take_back_pending |= bit(id);
        atomic_fetch_add(&S.counters.take_back_deferred, 1U);
    } else if (err != XIAOTAI_OK) {
        xiaotai_eventlog_error(SETTINGS_SOURCE, "take_back.failed", 0, err);
    }
    s_audio.applied[id] = value;
    s_audio.dirty &= ~bit(id);
    atomic_fetch_add(&S.counters.refused, 1U);
    xiaotai_settings_touch();
    xiaotai_eventlog_error(SETTINGS_SOURCE, "refused", 0, refusal);
    xiaotai_settings_emit(XIAOTAI_EVENT_SETTINGS_REFUSED, NAMES[id], refusal);
}

/* [owner] Records that the stored volume is expressed for the board's
 * ceiling: with the first volume this firmware stores. */
static void mark_ceiling(void)
{
    if (!s_audio.ceiling || s_audio.marked) return;
    const xiaotai_err_t err = xiaotai_preferences_store_int(XIAOTAI_SETTING_KEY_AUDIO_VOLUME_CEILING, s_audio.ceiling);
    if (err != XIAOTAI_OK) {
        xiaotai_eventlog_error(SETTINGS_SOURCE, "vol.ceiling", 0, err);
        return;
    }
    s_audio.marked = true;
}

/* [owner] Registry callback of every audio setting; ctx is the audio_id_t. */
static void audio_changed(const char *name, void *ctx)
{
    const audio_id_t id = (audio_id_t)(uintptr_t)ctx;
    /* The registry stored a volume (a change, a take-back, the conversion). */
    if (id == AUDIO_VOLUME) mark_ceiling();
    if (s_audio.taking_back) return;
    int32_t value = 0;
    if (xiaotai_setting_get_int(name, &value) != XIAOTAI_OK) return;
    atomic_fetch_add(&S.counters.changes, 1U);
    if (id == AUDIO_GUIDANCE) {
        /* Applied by the application: its feedback groups, its announcements. */
        xiaotai_settings_touch();
        xiaotai_settings_emit(XIAOTAI_EVENT_SETTINGS_CHANGED, name, value);
        return;
    }
    bool restore_preview_on_refusal = false;
    if (id == AUDIO_VOLUME && atomic_load(&S.previewing)) {
        const uint8_t previewed = atomic_load(&S.preview_volume);
        end_preview();
        if (previewed == (uint8_t)value) {
            /* The release of a slider at the previewed value: the engine has it. */
            s_audio.last_result = XIAOTAI_OK;
            s_audio.applied[id] = value;
            s_audio.dirty |= bit(id);
            xiaotai_settings_emit(XIAOTAI_EVENT_SETTINGS_CHANGED, name, value);
            return;
        }
        restore_preview_on_refusal = previewed != (uint8_t)s_audio.applied[AUDIO_VOLUME];
    }
    const bool preaccepted = (id == AUDIO_MIC_MUTED && s_audio.mic_preaccepted) ||
        accepts_before_save(id);
    const xiaotai_err_t err = preaccepted ? XIAOTAI_OK : apply(id, value);
    s_audio.last_result = err;
    if (err == XIAOTAI_OK) {
        s_audio.applied[id] = value;
        s_audio.dirty |= bit(id);
        s_audio.take_back_pending &= ~bit(id);
        xiaotai_settings_emit(XIAOTAI_EVENT_SETTINGS_CHANGED, name, value);
    } else {
        /* A preview changed the volume outside the registry. Taking back
         * the refused setting alone cannot replace that request. */
        if (restore_preview_on_refusal) restore_refused_preview(err);
        take_back(id, s_audio.applied[id], err);
    }
}

xiaotai_err_t xiaotai_settings_set_mic_mute(bool muted)
{
    if (!xiaotai_on_owner()) return XIAOTAI_ERR_INVALID_STATE;
    if (xiaotai_quiesced()) return XIAOTAI_ERR_BUSY;
    if (!s_audio.registered[AUDIO_MIC_MUTED]) return XIAOTAI_ERR_NOT_SUPPORTED;

    int32_t saved;
    xiaotai_err_t err = xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, &saved);
    if (err != XIAOTAI_OK) return err;
    xiaotai_media_status_t before;
    xiaotai_media_status(&before);
    err = apply(AUDIO_MIC_MUTED, muted);
    if (err != XIAOTAI_OK) {
        atomic_fetch_add(&S.counters.refused, 1U);
        xiaotai_eventlog_warn(SETTINGS_SOURCE, "refused", 0, err);
        return err;
    }
    if (saved == muted) {
        s_audio.applied[AUDIO_MIC_MUTED] = muted;
        return XIAOTAI_OK;
    }

    s_audio.mic_preaccepted = true;
    err = xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_MIC_MUTED, muted);
    s_audio.mic_preaccepted = false;
    if (err != XIAOTAI_OK && before.mic_muted != muted) {
        const xiaotai_err_t restored = apply(AUDIO_MIC_MUTED, before.mic_muted);
        if (restored != XIAOTAI_OK) xiaotai_eventlog_warn(SETTINGS_SOURCE, "restore.refused", 0, restored);
        atomic_store(&S.apply_error, err);
    }
    return err;
}

/* ---------------------------------------------------------- late refusals */

static bool late_kind(const char *text, audio_id_t *id)
{
    if (strcmp(text, "volume") == 0) *id = AUDIO_VOLUME;
    else if (strcmp(text, "mute") == 0) *id = AUDIO_SPEAKER_MUTED;
    else if (strcmp(text, "gain") == 0) *id = AUDIO_MIC_GAIN;
    else return false;
    return true;
}

/* [owner] The engine failed later to apply a request it had accepted. */
static void on_output_error(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    const xiaotai_err_t refusal = (xiaotai_err_t)event->args.value;
    atomic_fetch_add(&S.counters.output_errors, 1U);
    audio_id_t id;
    if (!late_kind(event->args.text, &id)) {
        /* "power" (the amplifier) has no setting: counted only. */
        xiaotai_eventlog_error(SETTINGS_SOURCE, "output.failed", 0, refusal);
        return;
    }
    if (!s_audio.registered[id]) return;
    if (id == AUDIO_VOLUME && atomic_load(&S.previewing)) {
        /* The failure belongs to the preview: it ends, the setting stays. */
        end_preview();
        xiaotai_eventlog_warn(SETTINGS_SOURCE, "preview.refused", 0, refusal);
        return;
    }
    atomic_store(&S.apply_error, refusal);
    xiaotai_settings_touch();
    xiaotai_media_status_t media;
    xiaotai_media_status(&media);
    const int32_t applied = id == AUDIO_VOLUME ? (int32_t)media.volume_applied
                            : id == AUDIO_SPEAKER_MUTED ? (media.speaker_muted_applied ? 1 : 0)
                                                        : (int32_t)media.input_gain_db;
    int32_t current = 0;
    if (xiaotai_setting_get_int(NAMES[id], &current) != XIAOTAI_OK || current == applied) return;
    if (!XIAOTAI_SETTINGS_TAKE_BACK_LATE_REFUSAL || !(s_audio.dirty & bit(id))) {
        atomic_fetch_add(&S.counters.late_kept, 1U);
        xiaotai_eventlog_warn(SETTINGS_SOURCE, "late.kept", 0, (int32_t)id);
        return;
    }
    take_back(id, applied, refusal);
}

/* [owner] A take-back the registry refused while quiesced is made on release. */
static void on_quiesce(const xiaotai_event_t *event, void *ctx)
{
    (void)ctx;
    if (event->args.value != 0 || !s_audio.take_back_pending) return;
    for (unsigned id = 0; id < AUDIO_COUNT; ++id) {
        if (!(s_audio.take_back_pending & bit((audio_id_t)id))) continue;
        s_audio.taking_back = true;
        const xiaotai_err_t err = xiaotai_setting_set_int(NAMES[id], s_audio.applied[id]);
        s_audio.taking_back = false;
        if (err == XIAOTAI_ERR_BUSY) continue;
        s_audio.take_back_pending &= ~bit((audio_id_t)id);
        xiaotai_settings_touch();
    }
}

/* -------------------------------------------------------------- shortcuts */

xiaotai_err_t xiaotai_settings_volume_step(int steps)
{
    if (!s_audio.registered[AUDIO_VOLUME]) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (steps == 0) return XIAOTAI_OK;
    atomic_fetch_add(&S.counters.steps, 1U);
    int32_t current = 0;
    xiaotai_err_t err = xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_VOLUME, &current);
    if (err != XIAOTAI_OK) return err;
    int64_t target = (int64_t)current + (int64_t)steps * xiaotai_settings_volume_step_percent();
    if (target < 0) target = 0;
    if (target > 100) target = 100;
    if (target == current) {
        atomic_fetch_add(&S.counters.steps_at_limit, 1U);
        return XIAOTAI_OK;
    }
    s_audio.last_result = XIAOTAI_OK;
    err = xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_VOLUME, (int32_t)target);
    if (err != XIAOTAI_OK) return err;
    if (s_audio.last_result != XIAOTAI_OK) return s_audio.last_result;
    if (XIAOTAI_SETTINGS_VOLUME_UP_UNMUTES && target > current && s_audio.registered[AUDIO_SPEAKER_MUTED]) {
        int32_t muted = 0;
        if (xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED, &muted) == XIAOTAI_OK && muted) {
            s_audio.last_result = XIAOTAI_OK;
            err = xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED, 0);
            if (err != XIAOTAI_OK) return err;
            return s_audio.last_result;
        }
    }
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_settings_volume_preview(uint8_t percent)
{
    if (!s_audio.registered[AUDIO_VOLUME]) return XIAOTAI_ERR_NOT_SUPPORTED;
    if (percent > 100U) return XIAOTAI_ERR_INVALID_ARG;
    /* Settings cannot change during maintenance; neither can what is heard. */
    if (xiaotai_quiesced()) return XIAOTAI_ERR_BUSY;
    atomic_fetch_add(&S.counters.previews, 1U);
    const xiaotai_err_t err = xiaotai_media_set_volume(percent);
    if (err != XIAOTAI_OK) {
        xiaotai_eventlog_warn(SETTINGS_SOURCE, "preview.refused", 0, err);
        return err;
    }
    atomic_store(&S.preview_volume, percent);
    atomic_store(&S.previewing, true);
    xiaotai_settings_touch();
    return XIAOTAI_OK;
}

void xiaotai_settings_volume_restore(void)
{
    if (!atomic_load(&S.previewing)) return;
    const uint8_t previewed = atomic_load(&S.preview_volume);
    end_preview();
    int32_t volume = 0;
    if (xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_VOLUME, &volume) != XIAOTAI_OK) return;
    if ((int32_t)previewed == volume) return;
    if (apply(AUDIO_VOLUME, volume) != XIAOTAI_OK) {
        xiaotai_eventlog_warn(SETTINGS_SOURCE, "restore.refused", 0, atomic_load(&S.apply_error));
    }
}

/* ----------------------------------------------------------------- intents */

static void on_volume_up(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    (void)xiaotai_settings_volume_step(1);
}

static void on_volume_down(const xiaotai_args_t *args, void *ctx)
{
    (void)args;
    (void)ctx;
    (void)xiaotai_settings_volume_step(-1);
}

static void on_volume_set(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    if (!args || args->value < 0 || args->value > 100) {
        xiaotai_eventlog_warn(SETTINGS_SOURCE, "intent.invalid", 0, args ? args->value : -1);
        return;
    }
    (void)xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_VOLUME, args->value);
}

/* args.value: 1 mute, 0 unmute, -1 toggle; anything else is ignored. */
static void set_mute(const char *name, const xiaotai_args_t *args)
{
    const int32_t request = args ? args->value : 1;
    int32_t value = request;
    if (request == -1) {
        int32_t current = 0;
        if (xiaotai_setting_get_int(name, &current) != XIAOTAI_OK) return;
        value = current ? 0 : 1;
    } else if (request != 0 && request != 1) {
        xiaotai_eventlog_warn(SETTINGS_SOURCE, "intent.invalid", 0, request);
        return;
    }
    (void)xiaotai_setting_set_int(name, value);
}

static void on_speaker_mute(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    set_mute(XIAOTAI_SETTING_AUDIO_SPEAKER_MUTED, args);
}

static void on_mic_mute(const xiaotai_args_t *args, void *ctx)
{
    (void)ctx;
    set_mute(XIAOTAI_SETTING_AUDIO_MIC_MUTED, args);
}

/* ----------------------------------------------------------- registration */

static xiaotai_err_t add(audio_id_t id, const char *key, xiaotai_setting_kind_t kind, int32_t min, int32_t max,
                         int32_t default_value)
{
    const xiaotai_setting_t setting = {
        .name = NAMES[id], .key = key, .kind = kind, .min = min, .max = max, .default_value = default_value,
        .changed = audio_changed, .ctx = (void *)(uintptr_t)id,
    };
    const xiaotai_err_t err = accepts_before_save(id)
        ? xiaotai_setting_register_int_admission(&setting, accept_audio_int) : xiaotai_setting_register(&setting);
    if (err != XIAOTAI_OK) return err;
    s_audio.registered[id] = true;
    xiaotai_settings_registered(NAMES[id]);
    /* Until start applies it, the stored value is what a refusal returns to. */
    (void)xiaotai_setting_get_int(NAMES[id], &s_audio.applied[id]);
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_settings_audio_register(void)
{
    const xiaotai_audio_output_t *output = xiaotai_hal_audio_output(0);
    const xiaotai_audio_input_t *input = xiaotai_hal_audio_input(0);
    xiaotai_err_t err = XIAOTAI_OK;
    if (output) {
        err = add(AUDIO_VOLUME, XIAOTAI_SETTING_KEY_AUDIO_VOLUME, XIAOTAI_SETTING_INT, 0, 100,
                  xiaotai_settings_volume_default());
        if (err == XIAOTAI_OK) {
            err = add(AUDIO_SPEAKER_MUTED, XIAOTAI_SETTING_KEY_AUDIO_SPEAKER_MUTED, XIAOTAI_SETTING_BOOL, 0, 1, 0);
        }
        if (err != XIAOTAI_OK) return err;
        atomic_store(&S.has_speaker, true);
        const uint8_t ceiling = output->info.volume_max_percent;
        if (ceiling > 0U && ceiling < 100U) {
            /* Read before start so the conversion needs no storage wait. A
             * device without the key saved its volume for the whole range. */
            int32_t from = 0, stored = 0;
            if (xiaotai_preferences_load_int(XIAOTAI_SETTING_KEY_AUDIO_VOLUME_CEILING, 0, &from) != XIAOTAI_OK ||
                from <= 0 || from > 100) {
                from = 100;
            }
            s_audio.ceiling = ceiling;
            s_audio.marked = from == ceiling;
            if (!s_audio.marked && xiaotai_preferences_load_int(XIAOTAI_SETTING_KEY_AUDIO_VOLUME,
                    xiaotai_settings_volume_default(), &stored) == XIAOTAI_OK) {
                s_audio.convert_from = (uint8_t)from;
            }
        }
    }
    if (input) {
        err = add(AUDIO_MIC_MUTED, XIAOTAI_SETTING_KEY_AUDIO_MIC_MUTED, XIAOTAI_SETTING_BOOL, 0, 1, 0);
        if (err != XIAOTAI_OK) return err;
        atomic_store(&S.has_microphone, true);
        const int8_t low = input->info.gain_min_db, high = input->info.gain_max_db;
        if (low < high) {
            int8_t gain = input->info.gain_db;
            if (gain < low) gain = low;
            if (gain > high) gain = high;
            S.gain_min_db = low;
            S.gain_max_db = high;
            err = add(AUDIO_MIC_GAIN, XIAOTAI_SETTING_KEY_AUDIO_MIC_GAIN, XIAOTAI_SETTING_INT, low, high, gain);
            if (err != XIAOTAI_OK) return err;
            atomic_store(&S.has_mic_gain, true);
        }
    }
    if (output) {
        err = add(AUDIO_GUIDANCE, XIAOTAI_SETTING_KEY_GUIDANCE_ENABLED, XIAOTAI_SETTING_BOOL, 0, 1,
                  xiaotai_settings_guidance_default() ? 1 : 0);
        if (err != XIAOTAI_OK) return err;
        atomic_store(&S.has_guidance, true);
        err = xiaotai_intent_handle(XIAOTAI_INTENT_AUDIO_VOLUME_UP, on_volume_up, NULL);
        if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_AUDIO_VOLUME_DOWN, on_volume_down, NULL);
        if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_AUDIO_VOLUME_SET, on_volume_set, NULL);
        if (err == XIAOTAI_OK) err = xiaotai_intent_handle(XIAOTAI_INTENT_AUDIO_SPEAKER_MUTE, on_speaker_mute, NULL);
        if (err != XIAOTAI_OK) return err;
    }
    if (input) {
        err = xiaotai_intent_handle(XIAOTAI_INTENT_AUDIO_MIC_MUTE, on_mic_mute, NULL);
        if (err != XIAOTAI_OK) return err;
    }
    if (output || input) {
        err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR, on_output_error, NULL);
        if (err == XIAOTAI_OK) err = XIAOTAI_EVENT_SUBSCRIBE_CONST(XIAOTAI_EVENT_QUIESCE, on_quiesce, NULL);
    }
    return err;
}

/* A volume saved for ceiling from (100: the whole range), played at ceiling
 * to with the same loudness: rounded to the volume step,
 * at least one step, at most 100. */
static int32_t converted_volume(int32_t volume, int32_t from, int32_t to)
{
    const int64_t step = xiaotai_settings_volume_step_percent() ? xiaotai_settings_volume_step_percent() : 1;
    const int64_t scaled = ((int64_t)volume * from * 2 + (int64_t)to * step) / ((int64_t)to * step * 2) * step;
    if (scaled < step) return (int32_t)(step < 100 ? step : 100);
    return scaled > 100 ? 100 : (int32_t)scaled;
}

/* [owner] A volume stored for another ceiling, converted once so the device
 * keeps its loudness; storing it records the board's ceiling. A failure is
 * retried at the next start. */
static void convert_stored_volume(void)
{
    int32_t volume = 0;
    if (!s_audio.convert_from || xiaotai_setting_get_int(XIAOTAI_SETTING_AUDIO_VOLUME, &volume) != XIAOTAI_OK) return;
    const int32_t converted = volume > 0 ? converted_volume(volume, s_audio.convert_from, s_audio.ceiling) : 0;
    if (converted != volume) {
        /* Not a user change: no admission; the caller applies it. */
        s_audio.taking_back = true;
        const xiaotai_err_t err = xiaotai_setting_set_int(XIAOTAI_SETTING_AUDIO_VOLUME, converted);
        s_audio.taking_back = false;
        if (err != XIAOTAI_OK) {
            xiaotai_eventlog_error(SETTINGS_SOURCE, "vol.convert", 0, err);
            return;
        }
        xiaotai_eventlog_note(SETTINGS_SOURCE, "vol.stored", 0, volume);
        xiaotai_eventlog_note(SETTINGS_SOURCE, "vol.convert", 0, converted);
    }
    /* Unchanged (0, already at 100): the stored value now means this ceiling. */
    mark_ceiling();
}

void xiaotai_settings_audio_prepare(void)
{
    convert_stored_volume();
    /* Declare the stored choices before any media worker opens its parts.
     * Keep deployed order and retain a stored value when its request fails. */
    for (unsigned id = 0; id < AUDIO_GUIDANCE; ++id) {
        if (!s_audio.registered[id]) continue;
        int32_t value = 0;
        if (xiaotai_setting_get_int(NAMES[id], &value) != XIAOTAI_OK) continue;
        s_audio.applied[id] = value;
        const xiaotai_err_t err = apply((audio_id_t)id, value);
        if (err != XIAOTAI_OK) xiaotai_eventlog_error(SETTINGS_SOURCE, "start.apply", 0, err);
    }
}

void xiaotai_settings_audio_reset(void) { memset(&s_audio, 0, sizeof(s_audio)); }
