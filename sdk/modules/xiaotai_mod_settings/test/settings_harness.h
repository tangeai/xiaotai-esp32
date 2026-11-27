// SPDX-License-Identifier: MIT
#pragma once
/* Test harness of the settings module: the real framework and event log on
 * the host port, fakes of the board (parts), the media engine and the
 * preferences service, and a probe module that records the module's events.
 * Functions named owner_* run the call on the owner task and wait until every
 * event it caused has been dispatched. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "xiaotai/framework.h"
#include "xiaotai/hal.h"
#include "xiaotai/modules/settings.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

/* ------------------------------------------------------------------ fakes */

typedef struct {
    bool output, input, display;
    bool dimmable;              /* the display's backlight has levels */
    int8_t gain_db, gain_min_db, gain_max_db;
    uint8_t volume_max_percent; /* the output's volume ceiling (0: none) */
} fake_board_t;

void fake_board_set(const fake_board_t *board);
void fake_preferences_set_pending(bool pending);

typedef enum {
    FAKE_MEDIA_VOLUME = 0,
    FAKE_MEDIA_SPEAKER_MUTED,
    FAKE_MEDIA_MIC_MUTED,
    FAKE_MEDIA_GAIN,
    FAKE_MEDIA_KINDS,
} fake_media_kind_t;

typedef struct {
    fake_media_kind_t kind;
    int32_t value;
} fake_media_call_t;

#define FAKE_MEDIA_CALLS_MAX 64U

void fake_media_reset(void);
void fake_media_clear_calls(void);
size_t fake_media_call_count(void);
const fake_media_call_t *fake_media_call(size_t index);
size_t fake_media_count(fake_media_kind_t kind);
/* Calls made from another task than the owner (must stay 0). */
uint32_t fake_media_off_owner(void);
int32_t fake_media_mic_setting_at_request(void);
#ifdef SETTINGS_TEST_REAL_PREFERENCES
uint32_t fake_media_stores_at_volume_request(void);
#endif
/* The next call of this kind returns err (once). */
void fake_media_fail_next(fake_media_kind_t kind, xiaotai_err_t err);
/* What the engine reports as applied in xiaotai_media_status. */
void fake_media_set_applied(uint8_t volume, bool speaker_muted, int8_t gain_db);

/* ------------------------------------------------------------------ probe */

typedef struct {
    char name[32];
    char text[XIAOTAI_ARG_TEXT_MAX];
    int32_t value;
} probe_event_t;

#define PROBE_EVENTS_MAX 64U

void probe_clear(void);
size_t probe_total(void);
const probe_event_t *probe_at(size_t index);
/* Events with this name (and text when not NULL). */
size_t probe_count(const char *name, const char *text);
/* Value of the last such event; -999 when none. */
int32_t probe_last_value(const char *name, const char *text);

/* ---------------------------------------------------------------- harness */

/* The stored value the framework's own store will load (namespace
 * "xt_settings", 4-byte integer), before harness_start. */
int harness_preset(const char *key, int32_t value);
/* Stored value now (after the store's queued writes); -999 when absent. */
int32_t harness_stored(const char *key);
/* Storage writes that reached the backend since the last clear. */
uint32_t harness_writes(void);
void harness_clear_writes(void);

/* Configures the module (NULL: defaults) and starts the framework with the
 * settings module and the probe; returns 0 when the start succeeded and every
 * event of the start was dispatched. */
int harness_start(const xiaotai_settings_config_t *config);
/* Stops the framework and forgets the module, the fakes' calls and storage. */
void harness_stop(void);
/* Everything queued so far, and what it queued, has been handled. */
int harness_settle(void);
/* Runs fn(ctx) on the owner task and settles. */
void harness_on_owner(void (*fn)(void *ctx), void *ctx);
/* Emits an event from the test task (as another engine would) and settles. */
void harness_emit(const char *name, int32_t value, const char *text);
/* The newest event-log entry from source "settings" with this label: its
 * value, or -999 when none was recorded since harness_start. */
int32_t harness_eventlog(const char *label);

/* ------------------------------------------------------ owner-task calls */

xiaotai_err_t owner_set(const char *name, int32_t value);
int32_t setting(const char *name);
xiaotai_err_t owner_step(int steps);
xiaotai_err_t owner_preview(uint8_t percent);
void owner_restore(void);
xiaotai_err_t owner_quiesce(void);
void owner_release(void);
xiaotai_err_t owner_intent(const char *name, int32_t value);
/* AT+STATUS settings as one text ("line1\nline2\n"). */
void owner_diag(char *out, size_t capacity);
