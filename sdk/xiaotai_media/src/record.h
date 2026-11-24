// SPDX-License-Identifier: MIT
#ifndef XIAOTAI_MEDIA_RECORD_H
#define XIAOTAI_MEDIA_RECORD_H

#include "xiaotai/media.h"

void media_record_init(void);
/* Host shutdown, after the capture writer stopped. An unfinished borrow is
 * completed with INVALID_STATE when the owner resumes dispatch. */
void media_record_reset(void);
bool media_record_active(void);
void media_record_status(xiaotai_media_status_t *status);
void media_record_stats(xiaotai_media_stats_t *stats);

/* The engine control lock arbitrates this against custom playback, session
 * and pre-roll admission. Capture only uses the private recording lock. */
xiaotai_err_t media_record_begin(const xiaotai_media_record_t *request, uint32_t rate_hz,
                                 int64_t now_us, uint32_t *id);
xiaotai_err_t media_record_begin_alaw(uint8_t *data, uint32_t samples, xiaotai_media_record_fn_t done,
                                      void *ctx, uint32_t rate_hz, int64_t now_us, uint32_t *id);
void media_record_end(uint32_t id, xiaotai_err_t result);
void media_record_feed(const int16_t *pcm, size_t samples, int64_t end_us, uint32_t lost_samples);
/* Nonblocking. True if the owner queue rejected a pending record update. */
bool media_record_service(void);

/* Private diagnostic recording: one byte per sample, at the clean capture
 * rate. Borrows data and obeys the same owner/permission rules as PCM. */
xiaotai_err_t media_session_record_alaw(uint8_t *data, uint32_t samples, xiaotai_media_record_fn_t done,
                                        void *ctx, uint32_t *id);

/* Implemented by the engine's control owner. */
xiaotai_err_t media_session_record_target(const xiaotai_media_record_t *request, uint8_t *alaw, uint32_t *id);
void media_session_record_stop(uint32_t id);
void media_session_record_output_free(void);

#endif
