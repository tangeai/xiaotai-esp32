#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    AUDIO_PLAYOUT_PROFILE_LOW_LATENCY = 0,
    AUDIO_PLAYOUT_PROFILE_ADAPTIVE_CALL,
    AUDIO_PLAYOUT_PROFILE_JITTER_SAFE,
} audio_playout_profile_t;

typedef enum {
    AUDIO_PLAYOUT_CONDITION_STARTUP = 0,
    AUDIO_PLAYOUT_CONDITION_STEADY,
    AUDIO_PLAYOUT_CONDITION_GUARDED,
    AUDIO_PLAYOUT_CONDITION_RECOVERY,
    AUDIO_PLAYOUT_CONDITION_LOCAL_PRESSURE,
} audio_playout_condition_t;

typedef enum {
    AUDIO_PLAYOUT_ACTION_NORMAL = 0,
    AUDIO_PLAYOUT_ACTION_EXPAND,
    AUDIO_PLAYOUT_ACTION_ACCELERATE,
    AUDIO_PLAYOUT_ACTION_FAST_ACCELERATE,
} audio_playout_action_t;

typedef struct {
    uint32_t received_ms;
    uint32_t played_ms;
    uint32_t underflows;
    uint32_t local_write_drop_ms;
    uint32_t hard_trim_ms;
} audio_playout_window_t;

typedef struct {
    audio_playout_action_t action;
    int16_t rate_adjust_permille;
    uint32_t target_delay_ms;
    uint32_t prebuffer_ms;
    uint32_t emergency_limit_ms;
} audio_playout_decision_t;

typedef struct {
    audio_playout_profile_t profile;
    audio_playout_condition_t condition;
    bool arrival_initialized;
    bool playback_started_once;
    bool rebuffering;
    uint32_t last_source_timestamp_ms;
    uint32_t last_arrival_ms;
    uint32_t packet_duration_ms;
    uint32_t jitter_q4_ms;
    uint32_t peak_timing_error_ms;
    uint32_t target_delay_ms;
    uint8_t stable_windows;
    uint8_t recovery_hold_windows;
} audio_playout_controller_t;

typedef struct {
    audio_playout_profile_t profile;
    audio_playout_condition_t condition;
    uint32_t packet_duration_ms;
    uint32_t target_delay_ms;
    uint32_t prebuffer_ms;
    uint32_t emergency_limit_ms;
    uint32_t jitter_ms;
    uint32_t peak_timing_error_ms;
} audio_playout_snapshot_t;

void audio_playout_controller_init(audio_playout_controller_t *controller,
                                   audio_playout_profile_t profile);
void audio_playout_controller_reset(audio_playout_controller_t *controller);
void audio_playout_controller_set_profile(audio_playout_controller_t *controller,
                                          audio_playout_profile_t profile);
void audio_playout_controller_observe_packet(audio_playout_controller_t *controller,
                                             uint32_t source_timestamp_ms,
                                             uint32_t arrival_ms,
                                             uint32_t packet_duration_ms);
void audio_playout_controller_note_playback_started(audio_playout_controller_t *controller);
void audio_playout_controller_note_underflow(audio_playout_controller_t *controller);
void audio_playout_controller_update_window(audio_playout_controller_t *controller,
                                            const audio_playout_window_t *window);
void audio_playout_controller_decide(const audio_playout_controller_t *controller,
                                     uint32_t buffered_ms,
                                     uint32_t chunk_ms,
                                     bool playback_started,
                                     audio_playout_decision_t *decision);
void audio_playout_controller_get_snapshot(const audio_playout_controller_t *controller,
                                           audio_playout_snapshot_t *snapshot);
const char *audio_playout_condition_name(audio_playout_condition_t condition);
