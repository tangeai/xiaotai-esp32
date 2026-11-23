// SPDX-License-Identifier: MIT
#pragma once
/* Wake-word engine: listens to the idle device's clean microphone audio and
 * reports when the wake phrase was spoken.
 *
 * The engine owns one low-priority task, the latest contiguous window of audio
 * and the acceptance rules (arming after start, cooldown, age, continuity).
 * Audio arrives through a tap of the media engine (xiaotai/media_capture.h),
 * so the engine never touches a board, a codec or the capture task's timing:
 * the capture task only copies a frame into a lock-free queue. The model that
 * scores a window is a registered implementation; a new phrase or a new
 * inference library is one more model, not a change here.
 *
 * What a detection leads to (starting a conversation, holding the pre-roll,
 * a sound, a screen) is decided by the module and the application above.
 *
 * Tuning names read from the media tuning table (xiaotai_media_tune), under
 * the engine's manifest name so the base module "wake" keeps "wake." for its
 * own: "wake_engine.threshold_milli" (default threshold), "wake_engine.raw"
 * (1: listen to the audio before gain control), "wake_engine.task_priority",
 * "wake_engine.task_core".
 *
 * Tags: [init] during init; [owner] the framework's owner task; [any] any
 * task. Nothing here may be called from an interrupt. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Threshold range, in thousandths of the model's probability. */
#define XIAOTAI_WAKE_THRESHOLD_MIN_MILLI 50U
#define XIAOTAI_WAKE_THRESHOLD_MAX_MILLI 990U
/* Default when the tuning table (xiaotai_media_tune) has no "wake_engine.threshold_milli". */
#define XIAOTAI_WAKE_THRESHOLD_DEFAULT_MILLI 900U

/* -------------------------------------------------------------------- model */

/* Where one scoring spent its time, and how much of the features it reused. */
typedef struct {
    uint32_t features_us, inference_us, head_us;
    uint16_t reused_frames, computed_frames;
} xiaotai_wake_timing_t;

/* A wake model: feature extraction, inference and scoring for one phrase.
 * Only the engine's task calls open(), detect() and close(), one at a time.
 * Its stack can be external. Use embedded or already mapped data, and
 * delegate any new Flash access or partition mapping to an internal-stack task.
 * The descriptor has static lifetime. */
typedef struct xiaotai_wake_model {
    const char *name;               /* "nihaoxiaotai" */
    /* Texts that mean the wake phrase (UTF-8), NULL-terminated; used by
     * development commands that inject a transcript. May be NULL. */
    const char *const *phrases;
    uint32_t rate_hz;               /* PCM the model takes: mono, signed 16-bit */
    uint32_t window_samples;        /* audio one scoring needs */
    /* The end of every window lies on a multiple of this many samples within
     * one contiguous stream, so the model may reuse features between windows;
     * 1: no alignment. */
    uint32_t grid_samples;
    uint32_t stride_samples;        /* new audio between two scorings */
    uint32_t stack_bytes;           /* stack detect() needs */
    /* Loads the model and checks it (memory by purpose, never internal RAM for
     * the bulk). XIAOTAI_ERR_NO_MEM, XIAOTAI_ERR_INVALID_ARG (damaged model
     * data), XIAOTAI_ERR_NOT_SUPPORTED (model does not fit this build's
     * inference library or tensor contract), XIAOTAI_FAIL. Everything taken is
     * released on failure; the engine retries later. */
    xiaotai_err_t (*open)(void **state);
    /* Probability (0..1) that window_samples of pcm end with the phrase.
     * stream identifies a contiguous run of audio and end_sample the position
     * of the window's end in it: with the same stream and a larger end_sample
     * on the grid, the model may reuse what it computed before. stream 0 is a
     * one-off window (the self-test) and invalidates what was kept. A failure,
     * or a probability that is not finite, is counted and never a detection.
     * timing may be NULL. */
    xiaotai_err_t (*detect)(void *state, const int16_t *pcm, uint32_t stream, uint64_t end_sample,
                            float *probability, xiaotai_wake_timing_t *timing);
    void (*close)(void *state);
} xiaotai_wake_model_t;

/* [init] The model registered last is the one used. The engine's platform
 * directory registers the built-in model when the build embeds it; host tests
 * register a scripted one. XIAOTAI_ERR_INVALID_ARG for an incomplete
 * descriptor (sizes of 0, a stride larger than the window, a missing function),
 * XIAOTAI_ERR_INVALID_STATE after start. */
xiaotai_err_t xiaotai_wake_model_register(const xiaotai_wake_model_t *model);
/* [any] The model in use; NULL when none is registered. */
const xiaotai_wake_model_t *xiaotai_wake_model(void);

/* --------------------------------------------------------------- detections */

typedef struct {
    uint32_t sequence;          /* 1 for the first detection since boot, then counting up */
    /* Capture time of the END of the accepted window (xiaotai_time_us clock):
     * pass it to xiaotai_media_preroll_hold to keep what was said after it. */
    int64_t captured_us;
    uint16_t score_milli;       /* probability in thousandths, rounded */
    uint16_t threshold_milli;   /* threshold in force when it was accepted */
} xiaotai_wake_detection_t;

/* [owner] Called for each accepted detection, in subscription order.
 *
 * The engine's task hands a detection to the owner task with xiaotai_run,
 * which never waits. While the owner's queue is full the engine keeps the one
 * accepted detection and posts it again at every poll (20 ms); a detection
 * whose window ended more than 1000 ms ago is dropped and counted
 * (detections_lost), because the pre-roll could no longer be held for it. */
typedef void (*xiaotai_wake_fn_t)(const xiaotai_wake_detection_t *detection, void *ctx);

/* [init] XIAOTAI_ERR_NO_MEM when the table is full. */
xiaotai_err_t xiaotai_wake_subscribe(xiaotai_wake_fn_t handler, void *ctx);

/* ------------------------------------------------------------------ control */

/* [owner] Wants (or stops wanting) the wake word. Off at boot. While on, the
 * engine listens whenever the media engine gives it audio: the device is idle,
 * the microphone is not muted and nothing records. The request is remembered
 * while the model is still loading. Turning it off or on starts a new window;
 * audio from before is never scored. XIAOTAI_ERR_NOT_SUPPORTED when no model
 * is registered, the board has no PCM capture, or the capture rate
 * (xiaotai_media_info().capture_rate_hz) is not the model's rate_hz: the
 * engine never resamples, and frames at another rate are dropped and counted
 * (wrong_rate) instead of being scored. */
xiaotai_err_t xiaotai_wake_listen(bool enabled);

/* [owner] Changes the threshold until the next restart (it is not stored: a
 * threshold belongs to a model, a microphone and a gain, and is validated at
 * the bench) and starts a new window. XIAOTAI_ERR_INVALID_ARG outside
 * XIAOTAI_WAKE_THRESHOLD_MIN_MILLI..MAX_MILLI, XIAOTAI_ERR_INVALID_STATE
 * while the model is not ready. */
xiaotai_err_t xiaotai_wake_set_threshold(uint16_t threshold_milli);

/* ------------------------------------------------------------------- status */

/* Counters since boot unless noted. */
typedef struct {
    bool ready;                 /* model loaded and self-test passed */
    bool wanted;                /* xiaotai_wake_listen(true) is in force */
    bool listening;             /* wanted, fed, armed and out of cooldown: a phrase spoken now is scored */
    xiaotai_err_t init_error;   /* last failure loading the model; XIAOTAI_OK when ready */
    uint16_t threshold_milli;
    uint32_t fed;               /* frames taken into the window */
    uint32_t dropped;           /* frames lost: the queue from the capture task was full */
    uint32_t resets;            /* windows started over */
    uint32_t stale;             /* resets caused by a time gap or a step back between frames */
    uint32_t skipped_unarmed;   /* frames ignored: old stream, not listening, before arming, in cooldown */
    uint32_t skipped_cooldown;  /* hits ignored: inside the cooldown */
    uint32_t inferences;
    uint32_t raw_hits;          /* scores at or above the threshold */
    uint32_t detections;        /* accepted and reported */
    uint32_t reject_stream;     /* hits dropped: listening or the stream changed during scoring */
    uint32_t reject_age;        /* hits dropped: window older than the age limit, or from the future */
    uint32_t reject_error;      /* scorings that failed */
    uint16_t last_score_milli;  /* latest score, rounded down */
    int64_t last_score_us;      /* when; 0: never */
    uint32_t detect_avg_us, detect_max_us;
    uint32_t detect_over_budget;/* scorings slower than one stride of audio */
    uint32_t window_age_max_ms; /* oldest window end at the moment its score was known */
    uint32_t features_max_us, inference_max_us, head_max_us;
    uint32_t reused_frames, computed_frames;
    uint32_t detections_lost;   /* accepted, never delivered: the owner's queue stayed full for 1000 ms */
    uint32_t wrong_rate;        /* frames dropped: their rate is not the model's */
} xiaotai_wake_status_t;

/* [any] Before the engine starts: zeroed, with the default threshold. */
void xiaotai_wake_status(xiaotai_wake_status_t *status);

/* ---------------------------------------------------------------- lifecycle */

/* Engine lifecycle (called by the framework through the generated tables).
 * init registers the read-only provider "wake_engine" for AT+STATUS, with
 * +WAKE_ENGINE and +WAKE_ENGINE-SCORE records. The base module "wake" owns
 * AT+WAKE TEST and SELFTEST; the engine's status, THRESHOLD and WATCH verbs
 * are a separate part of W4.
 * start creates the task; loading the model continues there and is retried
 * for good when it fails, so a device without a working wake word still
 * starts conversations by hand. */
xiaotai_err_t xiaotai_engine_wake_engine_init(void);
void xiaotai_engine_wake_engine_start(void);
/* Host tests: stop the task and forget models and subscribers. */
void xiaotai_wake_reset(void);

#ifdef __cplusplus
}
#endif
