// SPDX-License-Identifier: MIT
#pragma once
/* Settings module internals shared by its sources.
 *
 * settings.c owns configuration, the screen setting, status, statistics and
 * the lifecycle. The audio part is one of two sources, chosen by the build:
 * with_media.c (the product selects the media engine) or audio_none.c (it
 * does not); both implement the xiaotai_settings_audio_* functions below and
 * the public audio shortcuts of xiaotai/modules/settings.h.
 *
 * Everything that changes state runs on the framework's owner task. Fields a
 * status or statistics reader on another task may see are atomics; the
 * setting values themselves are read through the framework's registry, which
 * locks for its readers. */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include "xiaotai/framework.h"
#include "xiaotai/modules/settings.h"

/* Event-log source and diagnostics section. */
#define SETTINGS_SOURCE "settings"

/* Settings this module may register: five audio ones and the screen's two. */
#define SETTINGS_MAX 7U

typedef struct {
    _Atomic uint32_t changes, applied, refused, steps, steps_at_limit, previews, not_saved;
    /* XIAOTAI_EVENT_MEDIA_OUTPUT_ERROR seen (all four kinds). */
    _Atomic uint32_t output_errors;
    /* Late refusals that left the setting as it was (option off, or a value
     * applied at start). */
    _Atomic uint32_t late_kept;
    /* Take-backs the registry refused at first (quiesced) and retried. */
    _Atomic uint32_t take_back_deferred;
} settings_counters_t;

typedef struct {
    xiaotai_settings_config_t config;      /* as configured; 0 fields select defaults */
    bool initialized;
    /* Registered settings, in registration order (initial events, diagnostics). */
    const char *names[SETTINGS_MAX];
    size_t count;
    _Atomic bool has_speaker, has_microphone, has_mic_gain, has_screen, has_brightness, has_guidance;
    int8_t gain_min_db, gain_max_db;
    _Atomic bool previewing;
    _Atomic uint8_t preview_volume;
    _Atomic int apply_error;               /* xiaotai_err_t */
    _Atomic bool save_pending_seen;        /* last save_pending a status reader saw */
    _Atomic uint32_t revision;
    settings_counters_t counters;
} settings_state_t;

extern settings_state_t xiaotai_settings_state;

/* Effective configuration values (the zero fields resolved); the volume
 * default is public in xiaotai/modules/settings.h. */
bool xiaotai_settings_guidance_default(void);

/* A status field changed. */
void xiaotai_settings_touch(void);
/* Appends a registered setting's name (static storage) to the list. */
void xiaotai_settings_registered(const char *name);
/* Emits event with args.text = name and args.value = value. */
void xiaotai_settings_emit(const char *event, const char *name, int32_t value);

/* [init] Registers the audio settings the board allows, their intents and
 * subscriptions. Without the media engine nothing (XIAOTAI_OK). */
xiaotai_err_t xiaotai_settings_audio_register(void);
/* [owner] Applies the stored audio settings to the media engine. */
void xiaotai_settings_audio_prepare(void);
/* Forgets every audio state (preview, applied values, pending take-backs). */
void xiaotai_settings_audio_reset(void);

/* The "settings" diagnostics section. */
void xiaotai_settings_diag_report(const xiaotai_reply_t *out);
