// Adapted from the device-monitor P4 v1.5.3 playout controller (06583ea).
#include "audio_playout_controller.h"

#include <limits.h>
#include <stddef.h>
#include <string.h>

typedef struct {
    uint32_t initial_delay_ms;
    uint32_t base_delay_ms;
    uint32_t min_delay_ms;
    uint32_t max_delay_ms;
    uint32_t emergency_margin_ms;
    uint8_t stable_windows_before_decay;
} audio_playout_tuning_t;

/*
 * Profiles define the latency ceiling for each use case. Packet timing still
 * drives the target inside that ceiling, so a clean network starts quickly
 * while bursty delivery automatically trades a little latency for continuity.
 */
static const audio_playout_tuning_t s_tunings[] = {
    [AUDIO_PLAYOUT_PROFILE_LOW_LATENCY] = {
        .initial_delay_ms = 20,
        .base_delay_ms = 40,
        .min_delay_ms = 20,
        .max_delay_ms = 100,
        .emergency_margin_ms = 160,
        .stable_windows_before_decay = 3,
    },
    [AUDIO_PLAYOUT_PROFILE_ADAPTIVE_CALL] = {
        /* Device-call relay traces contain recoverable 300-500 ms delivery
         * holes under 200 +/- 100 ms delay and 5% loss. Starting at one
         * packet and capping the target at 220 ms made the PCM ring trim a
         * recovered burst, then underflow again. Keep the recovered audio in
         * the PSRAM ring and trade a bounded startup delay for continuity. */
        .initial_delay_ms = 120,
        .base_delay_ms = 140,
        .min_delay_ms = 120,
        .max_delay_ms = 560,
        .emergency_margin_ms = 320,
        .stable_windows_before_decay = 8,
    },
    [AUDIO_PLAYOUT_PROFILE_JITTER_SAFE] = {
        .initial_delay_ms = 120,
        .base_delay_ms = 160,
        .min_delay_ms = 120,
        .max_delay_ms = 320,
        .emergency_margin_ms = 320,
        .stable_windows_before_decay = 6,
    },
};

#define AUDIO_PLAYOUT_TARGET_QUANTUM_MS        10U
#define AUDIO_PLAYOUT_TARGET_ATTACK_MAX_MS     40U
#define AUDIO_PLAYOUT_TARGET_DECAY_MS          10U
#define AUDIO_PLAYOUT_UNDERFLOW_GUARD_MS       40U
#define AUDIO_PLAYOUT_RECOVERY_HOLD_WINDOWS    4U
#define AUDIO_PLAYOUT_STABLE_JITTER_MS         8U
#define AUDIO_PLAYOUT_STABLE_TIMING_ERROR_MS   10U
#define AUDIO_PLAYOUT_SOURCE_DELTA_MAX_MS      1000U
#define AUDIO_PLAYOUT_ARRIVAL_DELTA_MAX_MS     5000U
#define AUDIO_PLAYOUT_ACCELERATE_PERMILLE      12
#define AUDIO_PLAYOUT_FAST_ACCELERATE_PERMILLE 25
#define AUDIO_PLAYOUT_EXPAND_PERMILLE          (-12)

static bool audio_playout_profile_valid(audio_playout_profile_t profile)
{
    return profile == AUDIO_PLAYOUT_PROFILE_LOW_LATENCY ||
           profile == AUDIO_PLAYOUT_PROFILE_ADAPTIVE_CALL ||
           profile == AUDIO_PLAYOUT_PROFILE_JITTER_SAFE;
}

static const audio_playout_tuning_t *audio_playout_get_tuning(audio_playout_profile_t profile)
{
    if (!audio_playout_profile_valid(profile)) {
        profile = AUDIO_PLAYOUT_PROFILE_LOW_LATENCY;
    }
    return &s_tunings[profile];
}

static uint32_t audio_playout_clamp_u32(uint32_t value, uint32_t minimum, uint32_t maximum)
{
    if (value < minimum) {
        return minimum;
    }
    if (value > maximum) {
        return maximum;
    }
    return value;
}

static uint32_t audio_playout_round_up(uint32_t value, uint32_t quantum)
{
    if (quantum == 0U || value == 0U) {
        return value;
    }
    if (value > UINT32_MAX - (quantum - 1U)) {
        return UINT32_MAX;
    }
    return ((value + quantum - 1U) / quantum) * quantum;
}

static uint32_t audio_playout_jitter_ms(const audio_playout_controller_t *controller)
{
    return controller != NULL ? (controller->jitter_q4_ms + 8U) / 16U : 0U;
}

static uint32_t audio_playout_suggested_target_ms(const audio_playout_controller_t *controller)
{
    if (controller == NULL) {
        return 0U;
    }

    const audio_playout_tuning_t *tuning = audio_playout_get_tuning(controller->profile);
    uint32_t jitter_margin_ms = audio_playout_jitter_ms(controller) * 4U;
    if (jitter_margin_ms < controller->peak_timing_error_ms) {
        jitter_margin_ms = controller->peak_timing_error_ms;
    }

    uint32_t suggested_ms = tuning->base_delay_ms;
    if (jitter_margin_ms > UINT32_MAX - suggested_ms) {
        suggested_ms = UINT32_MAX;
    } else {
        suggested_ms += jitter_margin_ms;
    }
    if (suggested_ms < controller->packet_duration_ms) {
        suggested_ms = controller->packet_duration_ms;
    }
    suggested_ms = audio_playout_round_up(suggested_ms, AUDIO_PLAYOUT_TARGET_QUANTUM_MS);
    return audio_playout_clamp_u32(suggested_ms,
                                  tuning->min_delay_ms,
                                  tuning->max_delay_ms);
}

static uint32_t audio_playout_prebuffer_ms(const audio_playout_controller_t *controller)
{
    if (controller == NULL) {
        return 0U;
    }

    const audio_playout_tuning_t *tuning = audio_playout_get_tuning(controller->profile);
    uint32_t prebuffer_ms = controller->rebuffering ?
                            controller->target_delay_ms :
                            tuning->initial_delay_ms;
    if (prebuffer_ms < controller->packet_duration_ms) {
        prebuffer_ms = controller->packet_duration_ms;
    }
    return audio_playout_clamp_u32(prebuffer_ms,
                                  tuning->min_delay_ms,
                                  tuning->max_delay_ms);
}

static uint32_t audio_playout_emergency_limit_ms(const audio_playout_controller_t *controller)
{
    if (controller == NULL) {
        return 0U;
    }
    const audio_playout_tuning_t *tuning = audio_playout_get_tuning(controller->profile);
    if (controller->target_delay_ms > UINT32_MAX - tuning->emergency_margin_ms) {
        return UINT32_MAX;
    }
    return controller->target_delay_ms + tuning->emergency_margin_ms;
}

static void audio_playout_refresh_condition(audio_playout_controller_t *controller,
                                            bool local_pressure)
{
    if (controller == NULL) {
        return;
    }

    const audio_playout_tuning_t *tuning = audio_playout_get_tuning(controller->profile);
    if (local_pressure) {
        controller->condition = AUDIO_PLAYOUT_CONDITION_LOCAL_PRESSURE;
    } else if (controller->rebuffering || controller->recovery_hold_windows > 0U) {
        controller->condition = AUDIO_PLAYOUT_CONDITION_RECOVERY;
    } else if (!controller->playback_started_once) {
        controller->condition = AUDIO_PLAYOUT_CONDITION_STARTUP;
    } else if (controller->target_delay_ms > tuning->base_delay_ms +
                                             AUDIO_PLAYOUT_TARGET_QUANTUM_MS ||
               audio_playout_jitter_ms(controller) > AUDIO_PLAYOUT_STABLE_JITTER_MS) {
        controller->condition = AUDIO_PLAYOUT_CONDITION_GUARDED;
    } else {
        controller->condition = AUDIO_PLAYOUT_CONDITION_STEADY;
    }
}

static void audio_playout_attack_target(audio_playout_controller_t *controller,
                                        uint32_t suggested_ms)
{
    if (controller == NULL || suggested_ms <= controller->target_delay_ms) {
        return;
    }

    /* Increase protection quickly, then let stable one-second windows decay it. */
    uint32_t next_ms = suggested_ms;
    if (next_ms - controller->target_delay_ms > AUDIO_PLAYOUT_TARGET_ATTACK_MAX_MS) {
        next_ms = controller->target_delay_ms + AUDIO_PLAYOUT_TARGET_ATTACK_MAX_MS;
    }
    controller->target_delay_ms = next_ms;
    controller->stable_windows = 0U;
}

void audio_playout_controller_init(audio_playout_controller_t *controller,
                                   audio_playout_profile_t profile)
{
    if (controller == NULL) {
        return;
    }
    if (!audio_playout_profile_valid(profile)) {
        profile = AUDIO_PLAYOUT_PROFILE_LOW_LATENCY;
    }

    memset(controller, 0, sizeof(*controller));
    controller->profile = profile;
    controller->condition = AUDIO_PLAYOUT_CONDITION_STARTUP;
    controller->target_delay_ms = audio_playout_get_tuning(profile)->base_delay_ms;
}

void audio_playout_controller_reset(audio_playout_controller_t *controller)
{
    if (controller == NULL) {
        return;
    }
    audio_playout_profile_t profile = controller->profile;
    audio_playout_controller_init(controller, profile);
}

void audio_playout_controller_set_profile(audio_playout_controller_t *controller,
                                          audio_playout_profile_t profile)
{
    if (controller == NULL) {
        return;
    }
    if (!audio_playout_profile_valid(profile)) {
        profile = AUDIO_PLAYOUT_PROFILE_LOW_LATENCY;
    }
    if (controller->profile == profile) {
        return;
    }
    audio_playout_controller_init(controller, profile);
}

void audio_playout_controller_observe_packet(audio_playout_controller_t *controller,
                                             uint32_t source_timestamp_ms,
                                             uint32_t arrival_ms,
                                             uint32_t packet_duration_ms)
{
    if (controller == NULL) {
        return;
    }
    if (packet_duration_ms > 0U) {
        controller->packet_duration_ms = packet_duration_ms;
    }

    if (!controller->arrival_initialized) {
        controller->arrival_initialized = true;
        controller->last_source_timestamp_ms = source_timestamp_ms;
        controller->last_arrival_ms = arrival_ms;
        audio_playout_attack_target(controller,
                                    audio_playout_suggested_target_ms(controller));
        return;
    }

    uint32_t arrival_delta_ms = arrival_ms - controller->last_arrival_ms;
    int32_t source_delta_ms = (int32_t)(source_timestamp_ms -
                                        controller->last_source_timestamp_ms);
    controller->last_source_timestamp_ms = source_timestamp_ms;
    controller->last_arrival_ms = arrival_ms;

    if (arrival_delta_ms > AUDIO_PLAYOUT_ARRIVAL_DELTA_MAX_MS) {
        controller->jitter_q4_ms = 0U;
        controller->peak_timing_error_ms = 0U;
        return;
    }
    if (source_delta_ms <= 0 ||
        (uint32_t)source_delta_ms > AUDIO_PLAYOUT_SOURCE_DELTA_MAX_MS) {
        source_delta_ms = controller->packet_duration_ms > 0U ?
                          (int32_t)controller->packet_duration_ms :
                          (int32_t)arrival_delta_ms;
    }

    int64_t timing_error_ms = (int64_t)arrival_delta_ms - source_delta_ms;
    if (timing_error_ms < 0) {
        timing_error_ms = -timing_error_ms;
    }
    if (timing_error_ms > UINT32_MAX) {
        timing_error_ms = UINT32_MAX;
    }

    uint32_t error_ms = (uint32_t)timing_error_ms;
    int32_t jitter_error_q4 = (int32_t)(error_ms * 16U) -
                              (int32_t)controller->jitter_q4_ms;
    controller->jitter_q4_ms = (uint32_t)((int32_t)controller->jitter_q4_ms +
        (jitter_error_q4 >= 0 ? (jitter_error_q4 + 8) / 16 :
                                (jitter_error_q4 - 8) / 16));
    if (error_ms > controller->peak_timing_error_ms) {
        controller->peak_timing_error_ms = error_ms;
    }

    audio_playout_attack_target(controller,
                                audio_playout_suggested_target_ms(controller));
    audio_playout_refresh_condition(controller, false);
}

void audio_playout_controller_note_playback_started(audio_playout_controller_t *controller)
{
    if (controller == NULL) {
        return;
    }
    controller->playback_started_once = true;
    controller->rebuffering = false;
    audio_playout_refresh_condition(controller, false);
}

void audio_playout_controller_note_underflow(audio_playout_controller_t *controller)
{
    if (controller == NULL) {
        return;
    }

    const audio_playout_tuning_t *tuning = audio_playout_get_tuning(controller->profile);
    uint32_t guarded_target_ms = controller->target_delay_ms;
    if (guarded_target_ms <= UINT32_MAX - AUDIO_PLAYOUT_UNDERFLOW_GUARD_MS) {
        guarded_target_ms += AUDIO_PLAYOUT_UNDERFLOW_GUARD_MS;
    } else {
        guarded_target_ms = UINT32_MAX;
    }
    uint32_t suggested_ms = audio_playout_suggested_target_ms(controller);
    if (guarded_target_ms < suggested_ms) {
        guarded_target_ms = suggested_ms;
    }
    controller->target_delay_ms = audio_playout_clamp_u32(
        audio_playout_round_up(guarded_target_ms, AUDIO_PLAYOUT_TARGET_QUANTUM_MS),
        tuning->min_delay_ms,
        tuning->max_delay_ms);
    controller->rebuffering = true;
    controller->stable_windows = 0U;
    controller->recovery_hold_windows = AUDIO_PLAYOUT_RECOVERY_HOLD_WINDOWS;
    audio_playout_refresh_condition(controller, false);
}

void audio_playout_controller_update_window(audio_playout_controller_t *controller,
                                            const audio_playout_window_t *window)
{
    if (controller == NULL || window == NULL) {
        return;
    }

    const audio_playout_tuning_t *tuning = audio_playout_get_tuning(controller->profile);
    bool local_pressure = window->local_write_drop_ms > 0U || window->local_wait_ms > 0U;
    bool network_pressure = window->underflows > 0U ||
                            window->hard_trim_ms > 0U ||
                            controller->peak_timing_error_ms >
                                AUDIO_PLAYOUT_STABLE_TIMING_ERROR_MS;
    bool cadence_stable = window->received_ms + AUDIO_PLAYOUT_TARGET_QUANTUM_MS >=
                          window->played_ms;

    if (network_pressure || local_pressure) {
        controller->stable_windows = 0U;
        if (network_pressure) {
            audio_playout_attack_target(controller,
                                        audio_playout_suggested_target_ms(controller));
        }
    } else if (cadence_stable &&
               audio_playout_jitter_ms(controller) <= AUDIO_PLAYOUT_STABLE_JITTER_MS) {
        if (controller->stable_windows < UINT8_MAX) {
            controller->stable_windows++;
        }
    } else {
        controller->stable_windows = 0U;
    }

    if (controller->recovery_hold_windows > 0U) {
        controller->recovery_hold_windows--;
    } else if (controller->stable_windows >= tuning->stable_windows_before_decay) {
        uint32_t suggested_ms = audio_playout_suggested_target_ms(controller);
        if (controller->target_delay_ms > suggested_ms) {
            uint32_t next_ms = controller->target_delay_ms -
                               AUDIO_PLAYOUT_TARGET_DECAY_MS;
            controller->target_delay_ms = next_ms < suggested_ms ?
                                          suggested_ms : next_ms;
        }
        controller->stable_windows = 0U;
    }

    controller->peak_timing_error_ms = 0U;
    audio_playout_refresh_condition(controller, local_pressure);
}

void audio_playout_controller_decide(const audio_playout_controller_t *controller,
                                     uint32_t buffered_ms,
                                     uint32_t chunk_ms,
                                     bool playback_started,
                                     audio_playout_decision_t *decision)
{
    if (decision == NULL) {
        return;
    }
    memset(decision, 0, sizeof(*decision));
    if (controller == NULL) {
        return;
    }

    decision->target_delay_ms = controller->target_delay_ms;
    decision->prebuffer_ms = audio_playout_prebuffer_ms(controller);
    decision->emergency_limit_ms = audio_playout_emergency_limit_ms(controller);
    if (!playback_started || chunk_ms == 0U) {
        return;
    }

    uint32_t low_limit_ms = controller->target_delay_ms > chunk_ms ?
                            controller->target_delay_ms - chunk_ms :
                            chunk_ms;
    uint32_t high_limit_ms = controller->target_delay_ms + chunk_ms;

    /*
     * Positive adjustment consumes slightly more source PCM per output chunk;
     * negative adjustment consumes less. These small corrections avoid the
     * audible 20 ms jumps caused by dropping or duplicating whole chunks.
     */
    if (buffered_ms >= decision->emergency_limit_ms) {
        decision->action = AUDIO_PLAYOUT_ACTION_FAST_ACCELERATE;
        decision->rate_adjust_permille =
            AUDIO_PLAYOUT_FAST_ACCELERATE_PERMILLE;
    } else if (buffered_ms > high_limit_ms) {
        decision->action = AUDIO_PLAYOUT_ACTION_ACCELERATE;
        decision->rate_adjust_permille = AUDIO_PLAYOUT_ACCELERATE_PERMILLE;
    } else if (buffered_ms < low_limit_ms &&
               buffered_ms > chunk_ms) {
        decision->action = AUDIO_PLAYOUT_ACTION_EXPAND;
        decision->rate_adjust_permille = AUDIO_PLAYOUT_EXPAND_PERMILLE;
    }
}

void audio_playout_controller_get_snapshot(const audio_playout_controller_t *controller,
                                           audio_playout_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    memset(snapshot, 0, sizeof(*snapshot));
    if (controller == NULL) {
        return;
    }

    snapshot->profile = controller->profile;
    snapshot->condition = controller->condition;
    snapshot->packet_duration_ms = controller->packet_duration_ms;
    snapshot->target_delay_ms = controller->target_delay_ms;
    snapshot->prebuffer_ms = audio_playout_prebuffer_ms(controller);
    snapshot->emergency_limit_ms = audio_playout_emergency_limit_ms(controller);
    snapshot->jitter_ms = audio_playout_jitter_ms(controller);
    snapshot->peak_timing_error_ms = controller->peak_timing_error_ms;
}

const char *audio_playout_condition_name(audio_playout_condition_t condition)
{
    switch (condition) {
    case AUDIO_PLAYOUT_CONDITION_STARTUP:
        return "startup";
    case AUDIO_PLAYOUT_CONDITION_STEADY:
        return "steady";
    case AUDIO_PLAYOUT_CONDITION_GUARDED:
        return "guarded";
    case AUDIO_PLAYOUT_CONDITION_RECOVERY:
        return "recovery";
    case AUDIO_PLAYOUT_CONDITION_LOCAL_PRESSURE:
        return "local_pressure";
    default:
        return "unknown";
    }
}
