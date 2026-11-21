// SPDX-License-Identifier: MIT
#ifndef XIAOTAI_MEDIA_PREROLL_H
#define XIAOTAI_MEDIA_PREROLL_H

#include "xiaotai/media.h"

typedef struct media_preroll media_preroll_t;

/* Published with the prepared media capabilities until host teardown. */
media_preroll_t *media_preroll_current(void);

typedef struct {
    uint32_t token, generation;
    void (*lost)(uint32_t token, void *ctx);
    void *ctx;
} media_preroll_loss_t;

/* Construct only when the startup topology has clean PCM capture and reserves
 * history. An absent history represents an unsupported capability. */
xiaotai_err_t media_preroll_create(uint32_t rate_hz, uint32_t capacity_ms, media_preroll_t **out);
void media_preroll_destroy(media_preroll_t *history);
uint32_t media_preroll_capacity_ms(const media_preroll_t *history);

/* Capture-side, one block. A gap before this block starts a new idle history
 * and invalidates an old token. Failure to obtain the 2 ms lock is also a gap. */
xiaotai_err_t media_preroll_append(media_preroll_t *history, const int16_t *pcm, size_t samples,
                                   int64_t end_us, uint32_t lost_samples);
xiaotai_err_t media_preroll_append_granted(media_preroll_t *history, const int16_t *pcm, size_t samples,
                                           int64_t end_us, uint32_t lost_samples, uint32_t mic_epoch);
/* Epoch is the runtime microphone grant, captured with the owner's view.
 * This rejects a writer already delayed by capture effects or an AFE queue. */
void media_preroll_mic_transition(media_preroll_t *history, uint32_t mic_epoch, bool muted);

/* Owner-side admission. The engine checks microphone, session and recording
 * policy before calling this; no application callback runs under a lock. */
xiaotai_err_t media_preroll_hold(media_preroll_t *history, const xiaotai_media_preroll_t *request,
                                 int64_t now_us, uint32_t *token);
void media_preroll_release(media_preroll_t *history, uint32_t token);
uint32_t media_preroll_token(const media_preroll_t *history);
void media_preroll_status(const media_preroll_t *history, xiaotai_media_status_t *status);
void media_preroll_stats(const media_preroll_t *history, xiaotai_media_stats_t *stats);
xiaotai_err_t media_preroll_validate(media_preroll_t *history, uint32_t token, int64_t now_us);
xiaotai_err_t media_preroll_bind(media_preroll_t *history, uint32_t token, uint32_t generation, int64_t now_us);
/* Invalidation is nonblocking; a writer already holding the lock reconciles
 * it before its next operation. Closing an armed generation is a no-op. */
void media_preroll_discard(media_preroll_t *history, bool notify);
void media_preroll_close_binding(media_preroll_t *history, uint32_t generation);

/* Bound replay. Read up to one block. Finish only with less than one packet
 * remaining, returning that suffix and releasing replay in one operation. */
xiaotai_err_t media_preroll_read(media_preroll_t *history, uint32_t generation,
                                 int16_t *pcm, size_t samples, int64_t *start_us);
/* Copy to a fixed scratch block and consume only after a zero-wait queue
 * offer accepts it. `offer` must not run application code or wait. */
xiaotai_err_t media_preroll_replay(media_preroll_t *history, uint32_t generation, int16_t *scratch,
                                   size_t samples, bool (*offer)(const int16_t *, size_t, int64_t, void *),
                                   void *ctx);
xiaotai_err_t media_preroll_finish(media_preroll_t *history, uint32_t generation, int16_t *pcm,
                                   size_t packet_samples, size_t *samples, int64_t *start_us);

/* Owner polls for expiry and claims a pending loss exactly once; it calls
 * lost() only for an unbound token, or reports loss on the bound generation. */
bool media_preroll_take_loss(media_preroll_t *history, int64_t now_us, media_preroll_loss_t *event);
xiaotai_err_t media_preroll_poll_loss(media_preroll_t *history, int64_t now_us, bool *pending);
void media_preroll_post_retry(media_preroll_t *history);
/* Worker retries while the audio engine is active. No capture-side callback. */
bool media_preroll_service(void);
void media_preroll_dispatch_reset(void);
void media_session_preroll_lost(uint32_t generation);

#endif
