// SPDX-License-Identifier: MIT
#pragma once
/* Test doubles for the wake engine's host tests: the media engine's tap and
 * tuning functions (fake_media_tap.c), the framework's xiaotai_run
 * (fake_media_tap.c, beside the other fakes), a virtual clock, and a scripted
 * model (scripted_model.c). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/framework.h"
#include "xiaotai/media.h"
#include "xiaotai/media_capture.h"
#include "xiaotai/wake.h"

/* ---------------------------------------------------------------- the clock */

extern int64_t g_fake_now_us;
int64_t fake_now(void);

/* ------------------------------------------------------------ media fakes */

void fake_media_reset(void);
void fake_media_set_info(const xiaotai_media_info_t *info);
/* The tuning table xiaotai_media_param answers from (static storage). */
void fake_media_set_params(const xiaotai_media_param_t *params, size_t count);
/* The next xiaotai_media_tap_enable returns this (once). */
void fake_media_fail_enable(xiaotai_err_t err);
const xiaotai_media_tap_t *fake_media_tap(void);
unsigned fake_media_tap_registrations(void);
bool fake_media_tap_enabled(void);
unsigned fake_media_tap_enable_calls(void);
/* What the capture task does: open or close the gate, deliver a frame. */
void fake_media_gate(bool open);
void fake_media_pcm(const int16_t *pcm, size_t samples, uint32_t rate_hz, int64_t captured_us);

/* -------------------------------------------------- framework fake (owner) */

void fake_owner_reset(void);
/* xiaotai_run answers this until changed (XIAOTAI_OK: queue the job). */
void fake_owner_set_result(xiaotai_err_t result);
unsigned fake_owner_posts(void);    /* xiaotai_run calls, refused ones included */
unsigned fake_owner_queued(void);   /* jobs queued and not run yet */
/* Runs every queued job in order, as the owner task would; returns how many. */
unsigned fake_owner_run_all(void);

/* ---------------------------------------------------------- scripted model */

#define SCRIPT_MAX 64U
#define CALLS_MAX 256U

typedef struct {
    xiaotai_err_t result;
    float probability;
    int64_t duration_us;              /* advances the virtual clock during detect() */
    xiaotai_wake_timing_t timing;
    void (*during)(void);             /* runs inside detect(), after the clock moved */
} script_step_t;

typedef struct {
    uint32_t stream;
    uint64_t end_sample;
    int16_t first, last;              /* pcm[0] and pcm[window - 1] */
    bool all_zero;
    int64_t at_us;                    /* virtual time when detect() began */
} detect_call_t;

typedef struct {
    xiaotai_wake_model_t model;
    /* open() results in order; after the list, XIAOTAI_OK. */
    xiaotai_err_t open_results[SCRIPT_MAX];
    size_t open_result_count;
    /* detect() steps in order; after the list, default_step. */
    script_step_t steps[SCRIPT_MAX];
    size_t step_count;
    script_step_t default_step;
    /* What happened. */
    unsigned opens, closes, open_ok;
    int64_t open_at_us[SCRIPT_MAX];
    detect_call_t calls[CALLS_MAX];
    size_t call_count;
    bool is_open;
} scripted_model_t;

/* The one scripted model (its descriptor is g_script.model): window 16192,
 * grid 160, stride 2560, 16 kHz, stack 12288, two phrases; open() takes 64
 * bytes of BULK memory, close() gives them back. */
extern scripted_model_t g_script;
void scripted_model_reset(void);
