// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/media.h"
#include "xiaotai/modules/binding.h"
#include "xiaotai/platform.h"

extern unsigned fake_gets, fake_posts, fake_queries, fake_downloads, fake_unbound, fake_stops;
extern bool fake_online, fake_muted, fake_capture, fake_unbind_in_hook;
extern bool fake_speaker_muted;
extern bool fake_playback, fake_play_stopped;
extern bool fake_foreign_recording;
extern unsigned fake_plays;
extern bool fake_http_busy, fake_transfer_busy, fake_stop_holds;
extern bool fake_transfer_cancelled;
extern const char *fake_address;
extern xiaotai_err_t fake_endpoint_error;
extern uint32_t fake_generation, fake_recorded, fake_notice_revision;
extern xiaotai_media_record_t fake_capture_request;
extern xiaotai_platform_stream_t fake_stream;
extern size_t fake_body_size;
extern char fake_query_path[XIAOTAI_PLATFORM_PATH_MAX];
extern char fake_audio_path[XIAOTAI_PLATFORM_PATH_MAX];

void fake_policy_reply(const char *body, int http, int code, xiaotai_err_t error);
void fake_upload_done(const char *body, int http, bool sent, xiaotai_err_t error);
void fake_query_done(const void *body, size_t size, int64_t declared, size_t chunk, int http, xiaotai_err_t error);
void fake_download_done(int http, size_t received, xiaotai_err_t error);
bool fake_notice_channel(const char *type, const char *channel, const char *body, uint32_t generation);
void fake_notice(const char *type, const char *body, uint32_t generation);
void fake_record_done(xiaotai_err_t error, uint32_t samples);
void fake_play_done(xiaotai_err_t error);
xiaotai_err_t fake_binding_prepare(xiaotai_binding_prepared_fn_t done, void *ctx);

void check(bool value, const char *expression, const char *file, int line);
#define CHECK(expr) check(!!(expr), #expr, __FILE__, __LINE__)
