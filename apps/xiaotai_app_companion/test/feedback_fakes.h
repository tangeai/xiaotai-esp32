// SPDX-License-Identifier: MIT
#pragma once

#include "fakes.h"

typedef struct {
    bool enabled, audio_present, audio_ready, busy, guidance, rings[2], error;
    xiaotai_err_t audio_start_error;
    unsigned fired, cancelled, cancel_playing, released, announced, stopped, heard, holds_at_connected;
    unsigned by_effect[16];
    uint32_t last_generation, last_valid_ms;
    const char *last_effect, *waiting, *last_cancelled, *heard_effect;
    xiaotai_err_t startup_error, connected_error, announce_result;
    xiaotai_feedback_done_fn_t done;
    void *done_ctx;
} companion_feedback_fixture_t;

extern companion_feedback_fixture_t feedback_fixture;
void companion_fixture_audio_idle(void);
void companion_fixture_prompt_end(xiaotai_err_t result);
void companion_fixture_announcement_end(void);
unsigned companion_fixture_fire_count(const char *name);
unsigned companion_fixture_restart_holds(void);
