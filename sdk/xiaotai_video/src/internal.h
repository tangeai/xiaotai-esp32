// SPDX-License-Identifier: MIT
#pragma once
/* Video engine internals shared by its sources and its host tests: the engine
 * state, the backend registries, the requirement tables and the task plan.
 * Nothing here is part of the engine's API. */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "geometry.h"
#include "tuning.h"
#include "xiaotai/hal.h"
#include "xiaotai/port/mem.h"
#include "xiaotai/port/os.h"
#include "xiaotai/video.h"
#include "xiaotai/video_backend.h"

/* C only (C11 atomics): the engine's sources and its host tests. */

/* ------------------------------------------------------------- registries */

typedef enum {
    VIDEO_BACKEND_ENCODER = 0,
    VIDEO_BACKEND_DECODER,
    VIDEO_BACKEND_TRANSFORM,
    VIDEO_BACKEND_KINDS,
} video_backend_kind_t;

/* Registered backends of one kind, in registration order. */
size_t video_registry_count(video_backend_kind_t kind);
const xiaotai_video_encoder_t *video_encoder_at(size_t index);
const xiaotai_video_decoder_t *video_decoder_at(size_t index);
const xiaotai_video_transform_t *video_transform_at(size_t index);
/* Built-in fallback counts toward capacity without allocating a backend table. */
xiaotai_err_t video_registry_install_builtin(void);

/* Opens every XIAOTAI_VIDEO_DECODER_PERSISTENT decoder not opened yet, with
 * {codec, 0, 0} for the first video codec it matches at its own limit. A
 * decoder whose open fails, or which matches no video codec, is removed from
 * its registry (the capabilities then no longer list its codec) and the
 * failure is recorded in the event log. Owner task, before start. */
void video_registry_claim_persistent(void);
/* State of an opened persistent decoder; NULL when it is not one, or not
 * opened. */
void *video_registry_persistent_state(const xiaotai_video_decoder_t *decoder);
/* Closes the persistent decoders and frees the three tables. */
void video_registry_reset(void);

/* ------------------------------------------------------------ requirements */

/* A declared viewport. frames is the largest explicit count declared for the
 * display (0: none); default_frames records that a declaration left the
 * count to the engine, whose default (20 or 3) then also counts towards the
 * larger one when the pools are sized. */
typedef struct {
    xiaotai_video_viewport_t viewport;
    bool default_frames;
} video_viewport_entry_t;

typedef struct {
    xiaotai_video_profile_t *profiles;       /* XIAOTAI_VIDEO_PROFILES_MAX, BULK; NULL until the first */
    size_t profile_count;
    xiaotai_video_downlink_t downlinks[VIDEO_DOWNLINK_CODECS];   /* codec NONE: unused */
    video_viewport_entry_t *viewports;       /* XIAOTAI_VIDEO_VIEWPORTS_MAX, BULK; NULL until the first */
    size_t viewport_count;
} video_requirements_t;

/* Checks and normalizes an uplink profile as xiaotai_video_require_uplink()
 * describes: H.264 values clamped into their ranges, JPEG values checked.
 * XIAOTAI_ERR_INVALID_ARG for what cannot be normalized. */
xiaotai_err_t video_profile_normalize(const xiaotai_video_profile_t *profile, xiaotai_video_profile_t *out);
/* The declared profile equal to this one after normalization; NULL: not
 * declared (or invalid). */
const xiaotai_video_profile_t *video_require_find_uplink(const xiaotai_video_profile_t *profile);
/* The merged declaration for a codec; NULL when that codec was not declared. */
const xiaotai_video_downlink_t *video_require_downlink(xiaotai_video_codec_t codec);
/* The viewport declared for a display; NULL when none. */
const video_viewport_entry_t *video_require_viewport_for(uint8_t display);
/* True once anything was declared. */
bool video_require_any(void);

/* ------------------------------------------------------- uplink encoders */

/* The best encoder for codec on a camera at width x height, and in *pixel the
 * layout it would be given: one the camera delivers (only when direct_allowed:
 * the camera can open that size and nothing is turned), or one a registered
 * transform produces from a layout the camera delivers. Equal scores prefer
 * the camera's own layout (no transform), then the order YUYV422,
 * YUV420_PAIRED, I420, RGB565, RGB565_SWAPPED, RGB888. NULL when none
 * matches. The probe config carries the camera's colour range and row padding
 * (direct only), the profile's rate and quality fields (profile NULL: neutral
 * values: 15 fps, 1 Mbit/s, QP 10..51, 15 frames between key frames, quality
 * 80) and no output buffer, which match() must accept. */
const xiaotai_video_encoder_t *video_uplink_encoder(const xiaotai_video_profile_t *profile, xiaotai_video_codec_t codec,
                                                    const xiaotai_camera_desc_t *camera, uint16_t width,
                                                    uint16_t height, bool direct_allowed, xiaotai_video_pixel_t *pixel);
/* Video pixel layout of one XIAOTAI_CAMERA_* format bit; NONE for JPEG or an
 * unknown bit. */
xiaotai_video_pixel_t video_camera_pixel(uint32_t camera_format);

/* --------------------------------------------------------------- tasks */

typedef enum {
    VIDEO_TASK_SESSION = 0,
    VIDEO_TASK_UPLINK,
    VIDEO_TASK_DECODE_SOFTWARE,
    VIDEO_TASK_RELAY,
    VIDEO_TASK_CONVERT,
    VIDEO_TASK_DECODE_HARDWARE,
    VIDEO_TASK_KINDS,
} video_task_kind_t;

typedef struct {
    bool exists;
    xiaotai_task_config_t config;    /* entry and arg are filled at start */
} video_task_plan_entry_t;

typedef struct {
    video_task_plan_entry_t task[VIDEO_TASK_KINDS];
    bool uplink_encodable;
} video_task_plan_t;

/* Which engine tasks the declared requirements need, with their priority,
 * stack and core (the task table of the specification, section 2.2).
 * Nothing declared: no task. */
void video_task_plan(video_task_plan_t *plan);

/* One created engine task. */
typedef struct {
    xiaotai_task_t handle;
    xiaotai_signal_t exited;     /* given by the task just before it exits */
    atomic_bool stop;            /* set by xiaotai_video_reset() */
    bool created;
} video_task_t;

/* Copied as a value between the owner, session task and camera task. The
 * revision distinguishes reopened sessions even when a link is reused. */
typedef struct {
    uint32_t generation, revision;
    bool has_uplink, has_downlink;
    xiaotai_video_profile_t uplink;
    const xiaotai_video_profile_t *declared_uplink;
    xiaotai_video_downlink_t downlink;
    xiaotai_video_viewport_t viewport;
    xiaotai_video_placement_t placement;
    const xiaotai_video_link_t *link;
    uint8_t camera, display;
    uint16_t width, height;
} video_session_config_t;

/* Installed at start by the compressed-frame pool/decoder. Fallible begin
 * rolls back its own partial state. receive copies once into its pool and
 * verifies revision on enqueue independently of the session's initial check. */
typedef struct {
    xiaotai_err_t (*begin)(const video_session_config_t *config,
                           const xiaotai_video_transform_config_t *plan);
    void (*stop)(uint32_t revision);
    bool (*receive)(uint32_t revision, const xiaotai_video_packet_t *packet);
} video_session_receiver_t;

void video_session_install_receiver(const video_session_receiver_t *receiver);
bool video_session_has_receiver(void);
typedef struct {
    uint32_t discarded, rejected, decoded, failed, gaps, depth;
    uint16_t width, height;
} video_downlink_update_t;
void video_session_downlink_update(uint32_t revision, const video_downlink_update_t *update);
bool video_session_downlink_buffering(uint32_t revision);
bool video_session_downlink_current(uint32_t revision);
bool video_session_downlink_config(uint32_t revision, video_session_config_t *out);
void video_session_downlink_converted(uint32_t revision, bool success);
void video_session_downlink_presented(uint32_t revision);
void video_session_downlink_need_key(uint32_t revision);
void video_session_downlink_quarantine(uint32_t revision, bool quarantined);
void video_session_downlink_flags(uint32_t revision, bool waiting_key, bool unsupported);
void video_session_downlink_presentable(uint32_t revision, bool presentable);
bool video_session_uplink_request(video_session_config_t *out);
bool video_session_uplink_current(uint32_t revision);
void video_session_uplink_report(uint32_t revision, xiaotai_video_phase_t phase, xiaotai_err_t error);
void video_session_uplink_stats(uint32_t revision, const xiaotai_video_uplink_status_t *snapshot);
void video_session_uplink_luma(uint32_t revision, video_luma_t luma);
void video_status_with_luma(xiaotai_video_status_t *status, video_luma_t *luma);
bool video_session_uplink_take_key(uint32_t revision);
uint32_t video_session_uplink_bitrate(uint32_t revision);
uint32_t video_session_step(int64_t now_us);
void video_session_task(void *arg);
struct xiaotai_eventlog_topic;
void video_session_set_health_topics(struct xiaotai_eventlog_topic *up, struct xiaotai_eventlog_topic *down);
void video_session_reset(void);
bool video_session_diagnostics_busy(void);
bool video_session_capture_current(uint32_t *generation, uint32_t *revision);
bool video_session_capture_key(uint32_t revision);
xiaotai_err_t video_at_register(void);

/* ------------------------------------------------------------ engine state */

typedef struct {
    bool initialized;
    atomic_bool started;
    bool restore_initial_quality_on_resolution_reopen;
    bool buffer_independent_frames;
    bool probe_enabled;
    /* Directions start could provide; sessions that need a missing one get
     * XIAOTAI_ERR_NOT_SUPPORTED. */
    bool uplink_available;
    bool h264_unavailable;
    bool downlink_available;
    video_requirements_t requirements;
    video_task_t tasks[VIDEO_TASK_KINDS];
    /* Guards status. Initialized by the first entry point that writes it. */
    xiaotai_critical_t lock;
    bool lock_ready;
    xiaotai_video_status_t status;
    video_luma_t uplink_luma;  /* protected by the status lock */
    atomic_uint_least32_t remote_generation;
} video_engine_t;

extern video_engine_t video_engine;

/* True once xiaotai_engine_video_start() has run. */
static inline bool video_started(void) { return atomic_load(&video_engine.started); }
