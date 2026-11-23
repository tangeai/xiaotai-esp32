// SPDX-License-Identifier: MIT
#ifndef XIAOTAI_MEDIA_CLIP_PLAYOUT_H
#define XIAOTAI_MEDIA_CLIP_PLAYOUT_H

#include "sound_decode.h"
#include "play_stream.h"
#include "xiaotai/hal.h"
#include "xiaotai/media.h"

typedef struct media_clip_player media_clip_player_t;

typedef struct {
    media_sound_cursor_t clips[XIAOTAI_MEDIA_PLAY_CLIPS_MAX];
    media_stream_buffer_t *stream;
    xiaotai_media_play_fn_t done;
    void *ctx;
    uint32_t generation, flags;
    uint16_t repeat, gap_ms;
    uint8_t clip_count;
    xiaotai_play_priority_t priority;
} media_clip_request_t;

/* One output worker per engine. Create before the worker starts; destroy only
 * after it stops. All PCM scratch and registered decoder states are reserved
 * here, never during play. PCM and A-law need no decoder instance. */
xiaotai_err_t media_clip_player_create(const xiaotai_audio_output_t *output, media_clip_player_t **player);
void media_clip_player_destroy(media_clip_player_t *player);
uint32_t media_clip_player_decoder_stack_bytes(const media_clip_player_t *player);

/* Prepare runs outside the engine control lock. Submit copies the validated
 * request under a short lock. Data of an accepted request remains borrowed
 * until its owner-task done callback. Admission does not call the output. */
xiaotai_err_t media_clip_request_prepare(const media_clip_player_t *player, const xiaotai_media_play_t *request,
                                         media_clip_request_t *prepared);
/* A successful submit takes the stream reference created during prepare.
 * Discard only rejected preparations, outside the runtime lock. */
xiaotai_err_t media_clip_stream_prepare(const media_clip_player_t *player, const xiaotai_media_stream_t *request,
                                        media_clip_request_t *prepared);
void media_clip_request_discard(media_clip_request_t *prepared);
xiaotai_err_t media_clip_player_submit(media_clip_player_t *player, const media_clip_request_t *request,
                                       uint32_t *id);
media_stream_buffer_t *media_clip_player_find_stream(media_clip_player_t *player, uint32_t id);
uint32_t media_clip_player_id(media_clip_player_t *player);
bool media_clip_player_needs_work(media_clip_player_t *player);
void media_clip_player_cancel(media_clip_player_t *player, uint32_t id, xiaotai_err_t why);
void media_clip_player_cancel_generation(media_clip_player_t *player, uint32_t generation);
void media_clip_player_enable_mixer(media_clip_player_t *player, bool enabled); /* before worker start */
void media_clip_player_capture_clock(media_clip_player_t *player, bool enabled); /* before worker start */
bool media_clip_player_over_session(media_clip_player_t *player, uint32_t id);
void media_clip_player_cancel_plain(media_clip_player_t *player);

/* Worker only. Pass the id whose output power was made ready; 0 processes
 * cancellations and callback posting only. An outdated id never writes.
 * At most one interleaved PCM block is written. Returns wait in ms. */
uint32_t media_clip_player_step(media_clip_player_t *player, uint32_t powered_id, xiaotai_err_t power_result);

/* Same output worker, inside the normal downlink block. Mix does not write
 * the part; pass its token back after the single combined write. */
uint32_t media_clip_player_mix(media_clip_player_t *player, uint32_t id, int16_t *mono, size_t frames);
void media_clip_player_mix_written(media_clip_player_t *player, uint32_t token, xiaotai_err_t result);

/* Runtime admission/window; application wrappers never access the player
 * pointer outside its owner. Acquired stream refs outlive their admission. */
xiaotai_err_t media_session_play_stream(const xiaotai_media_stream_t *request, uint32_t *id);
media_stream_buffer_t *media_session_find_stream(uint32_t id);
void media_session_wake_playout(void);

#endif
