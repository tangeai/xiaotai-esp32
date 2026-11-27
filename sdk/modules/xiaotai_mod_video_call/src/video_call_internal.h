// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/modules/video_call.h"
#include "xiaotai/video.h"

typedef struct {
    const char *scene;
    bool (*match)(uint32_t link);
    xiaotai_err_t (*dial_contact)(size_t index);
    xiaotai_video_codec_t peer_codec;
    /* The peer may be without that codec's encoder or decoder and use JPEG. */
    bool peer_may_use_jpeg;
    bool profile_as_arrays;
    xiaotai_err_t (*subscribed)(uint32_t link);
} video_call_host_t;

typedef struct {
    xiaotai_err_t (*show)(uint8_t display, uint32_t link);
    void (*clear)(uint8_t display, uint32_t link);
} video_call_ui_t;

extern const video_call_host_t *const video_call_hosts[];
extern const video_call_ui_t *const video_call_ui;

typedef struct {
    const video_call_host_t *host;
    const xiaotai_video_call_scene_t *scene;
    uint8_t camera;
    const xiaotai_video_profile_t *uplink;
    const xiaotai_video_downlink_t *downlink;
    const xiaotai_video_downlink_t *other;      /* the peer's second codec; NULL: none */
} video_call_scene_part_t;

bool video_call_profile_config_valid(const xiaotai_video_call_scene_t *scene);
int video_call_profile_write(char *out, size_t capacity, void *ctx);
xiaotai_err_t video_call_at_init(void);
