// SPDX-License-Identifier: MIT
#pragma once
/* Media governor: keeps an H.264 uplink inside what the link can carry.
 *
 * Three signals reach it: the link's bitrate target, a failed send
 * (backpressure) and the passing of time. It decides one number, the encoder's
 * bitrate, with hysteresis so that the encoder is not retuned for noise:
 * reductions are prompt, recoveries slow and stepwise. Picture size and frame
 * rate are never changed by it.
 *
 * Pure logic on caller-owned state: no allocation, no clock, no lock, no
 * logging. The caller passes the time (xiaotai_time_us()) and serializes calls
 * on one governor; the engine runs it on its session task and reads the
 * backpressure flag from its camera task under its own lock. Every function
 * below is therefore [any, rt] (xiaotai/framework.h), with that
 * serialization left to the caller. */
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Range offered to the link for a profile. */
#define XIAOTAI_VIDEO_BITRATE_COMPACT_PIXELS (640u * 480u)  /* up to this many pixels the compact floor applies */
#define XIAOTAI_VIDEO_BITRATE_COMPACT_FLOOR_BPS 96000u
#define XIAOTAI_VIDEO_BITRATE_LARGE_FLOOR_BPS 750000u
#define XIAOTAI_VIDEO_BITRATE_FLOOR_DIVISOR 4u               /* the floor is at least bitrate / 4 */
#define XIAOTAI_VIDEO_BITRATE_START_PERCENT 80u              /* start = min + 80 % of (max - min) */

/* Convergence on the link's target. */
#define XIAOTAI_VIDEO_BITRATE_MIN_STEP_BPS 16000u            /* smaller changes are ignored, */
#define XIAOTAI_VIDEO_BITRATE_MIN_STEP_PERCENT 10u           /* as are those under 10 % of the current bitrate */
#define XIAOTAI_VIDEO_BITRATE_DECREASE_INTERVAL_MS 1000u     /* between two reductions */
#define XIAOTAI_VIDEO_BITRATE_EMERGENCY_PERCENT 25u          /* a reduction of at least 25 % is never delayed */
#define XIAOTAI_VIDEO_BITRATE_RECOVERY_HOLD_MS 5000u         /* no increase this long after a reduction */
#define XIAOTAI_VIDEO_BITRATE_RECOVERY_INTERVAL_MS 3000u     /* between two increases */
#define XIAOTAI_VIDEO_BITRATE_RECOVERY_STEP_PERCENT 15u      /* an increase is at most 15 % of the current bitrate, */
#define XIAOTAI_VIDEO_BITRATE_RECOVERY_MIN_STEP_BPS 64000u   /* but may always be this much */

/* After a failed send the camera task takes no picture for this long. */
#define XIAOTAI_VIDEO_BACKPRESSURE_HOLD_MS 80u

/* In the order the link's set_bitrate_range takes them. */
typedef struct {
    uint32_t min_bps;
    uint32_t start_bps;   /* min < start < max */
    uint32_t max_bps;
} xiaotai_video_bitrate_range_t;

/* The range for an encoded picture of width x height at bitrate_bps:
 *   max   = bitrate_bps
 *   min   = the larger of bitrate_bps / 4 and the floor for the picture size
 *           (compact or large), kept below max
 *   start = min + 80 % of (max - min), kept strictly between min and max
 * bitrate_bps must be at least 3. */
void xiaotai_video_bitrate_range(uint16_t width, uint16_t height, uint32_t bitrate_bps,
                                 xiaotai_video_bitrate_range_t *range);

/* The state of one uplink. Callers keep it (in static storage or a session
 * struct) and treat the fields as private; they are public only so that no
 * allocation is needed. */
typedef struct {
    xiaotai_video_bitrate_range_t range;
    uint32_t bitrate_bps;             /* what the encoder should use now */
    uint32_t requested_bps;           /* the link's latest target, clamped to the range; 0: none yet */
    int64_t decrease_not_before_us;   /* 0: no reduction is pending a wait */
    int64_t increase_not_before_us;
    int64_t backpressure_until_us;    /* 0: never marked */
    uint32_t backpressure_events;
} xiaotai_video_governor_t;

/* Start of a session or a change of profile: the bitrate is the profile's
 * (range.max_bps), there is no target, and waits and backpressure are cleared. */
void xiaotai_video_governor_reset(xiaotai_video_governor_t *governor, uint16_t width, uint16_t height,
                                  uint32_t bitrate_bps);

/* A target from the link (the engine also passes range.start_bps once the link
 * accepted the range, so the encoder starts where the link does). The target is
 * clamped to the range and remembered; a target below the current bitrate
 * forbids increases for the recovery hold. Then one convergence step runs (see
 * xiaotai_video_governor_step). target_bps 0 is ignored. Returns true when
 * bitrate_bps changed: the caller reconfigures the encoder. */
bool xiaotai_video_governor_target(xiaotai_video_governor_t *governor, uint32_t target_bps, int64_t now_us);

/* One convergence step towards the remembered target; call it about every
 * 200 ms when no new target arrived. Nothing changes when there is no target,
 * when the target equals the bitrate, or when the difference is below the
 * minimum step and the target is not an end of the range.
 *   Down: the bitrate becomes the target at once, unless a reduction happened
 *   within the decrease interval and this one is not an emergency. Afterwards
 *   increases wait for the recovery hold.
 *   Up: not before the recovery hold or interval has passed; then by at most
 *   one recovery step. Afterwards the next increase waits the recovery interval.
 * Returns true when bitrate_bps changed. */
bool xiaotai_video_governor_step(xiaotai_video_governor_t *governor, int64_t now_us);

/* How far below the profile the uplink runs: 0 at the profile's bitrate, 1 at
 * 85 % or more of it, 2 at 70 % or more, 3 below. For diagnostics. */
uint8_t xiaotai_video_governor_level(const xiaotai_video_governor_t *governor);

/* A send failed for a reason other than "not ready": pause capture for the
 * backpressure hold from now. */
void xiaotai_video_governor_backpressure(xiaotai_video_governor_t *governor, int64_t now_us);
/* True while the hold of the latest mark lasts. */
bool xiaotai_video_governor_backpressured(const xiaotai_video_governor_t *governor, int64_t now_us);

#ifdef __cplusplus
}
#endif
