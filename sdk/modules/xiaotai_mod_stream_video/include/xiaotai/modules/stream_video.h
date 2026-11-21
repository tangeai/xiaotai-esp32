// SPDX-License-Identifier: MIT
#pragma once
/* Base module "stream_video": video on the web real-time stream.
 *
 * It rides on the activity of the module "stream" (xiaotai/modules/stream.h):
 * while a viewer's stream is active it sends the camera to the viewer, when
 * the viewer subscribes, and, where the product shows pictures, takes the
 * viewer's video. It is a session extension of the stream activity
 * (xiaotai/framework.h): its attachment contributes the video part of the
 * stream's link (XIAOTAI_LINK_SLOT_VIDEO) and the video members of the
 * "stream" profile scene (XIAOTAI_PLATFORM_SLOT_SCENE), so the admitted link
 * carries video and the device profile announces it, without any API of the
 * stream module (xiaotai/modules/stream.h, "Video"). It opens a session of the
 * video engine (xiaotai/video.h) over the stream's link and closes it when the
 * stream ends. A product without this module has a stream without video: the
 * viewer's video subscription is refused and the profile says so.
 *
 * It draws nothing itself. Where the product selects the UI engine
 * (xiaotai/ui.h; an optional dependency), the module installs the viewer's
 * picture as the video source of `display` for the stream's generation when
 * XIAOTAI_EVENT_STREAM_VIDEO_REMOTE reports it ready (args.value 1) and clears
 * it when the picture or the stream ends: the UI engine's built-in source over
 * the video engine's presenter functions, the same one the video call module
 * installs. Whether a view with a video region is shown stays the
 * application's decision. Without the UI engine whoever presents the picture
 * fetches it with the video engine's presenter functions for the reported
 * generation. The viewport it is laid out for is declared to the video engine
 * by the product (or named in the configuration below).
 *
 * A camera or encoder failure never ends the stream: audio goes on and the
 * failure is reported as XIAOTAI_EVENT_STREAM_VIDEO_UPLINK.
 *
 * Generations: the stream's link generation is the video session's generation.
 *
 * Tags are those of xiaotai/framework.h: [init], [owner], [any], [rt]. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"
#include "xiaotai/video.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ configuration */

/* What a product chooses. A zeroed struct selects every default. Pointers are
 * not copied: what they point to has static lifetime. */
typedef struct {
    /* What is encoded for the viewer. NULL: chosen from what the video engine
     * can encode on this device: the H.264 profile for a phone or browser
     * (XIAOTAI_VIDEO_PROFILE_H264_SENSOR) where an H.264 encoder exists, else
     * the JPEG stream profile (XIAOTAI_VIDEO_PROFILE_JPEG_STREAM). */
    const xiaotai_video_profile_t *uplink;
    /* true: the camera is never sent (a product that only shows the viewer). */
    bool no_uplink;
    /* What the viewer sends. NULL: H.264 as XIAOTAI_VIDEO_DOWNLINK_H264_COMPACT
     * describes it. Used only when the viewer's video is taken at all: the
     * video engine has a decoder for the codec and a viewport is declared for
     * `display` (by `viewport` below or by another component). */
    const xiaotai_video_downlink_t *downlink;
    /* true: the viewer's video is never taken. */
    bool no_downlink;
    /* Camera part and display part used; 0 is the first of each. display is
     * also the one whose video source this module installs (UI engine). */
    uint8_t camera;
    uint8_t display;
    /* Viewport this module declares for `display`. NULL: it declares none
     * and uses the one another component declares. The video engine keeps one
     * viewport per display and refuses a different second one (init then
     * returns its XIAOTAI_ERR_INVALID_STATE): give the display's plain
     * viewport here, as xiaotai_video_viewport_for_display fills it, and lay
     * the viewer's picture out with `placement`. */
    const xiaotai_video_viewport_t *viewport;
    /* How the viewer's picture is turned and placed on that viewport
     * (xiaotai_video_session_t.placement). NULL: the viewport's default. */
    const xiaotai_video_placement_t *placement;
    /* Presentation hints sent to the viewer in the device profile. They
     * describe how the product wants its picture shown, not the encoder.
     * NULL: "4:3" and "contain". Static strings of letters, digits and ":.". */
    const char *aspect_ratio;
    const char *object_fit;
    bool mirror_horizontal, mirror_vertical;
    /* Use an unquoted positive JSON decimal for aspect_ratio on products
     * whose deployed profile uses that wire type. Otherwise it is a string. */
    bool aspect_ratio_number;
} xiaotai_stream_video_config_t;

/* [glue] By the product glue, before xiaotai_framework_start(): the
 * module reads the configuration in its own init, where it declares its needs
 * to the video engine, and that runs before any application's init. The struct
 * is copied. NULL: all defaults. XIAOTAI_ERR_INVALID_ARG for a hint with other
 * characters, a non-decimal ratio with aspect_ratio_number, a viewport for
 * another display, or no_uplink together with no_downlink;
 * XIAOTAI_ERR_INVALID_STATE once the module is initialized. */
xiaotai_err_t xiaotai_stream_video_configure(const xiaotai_stream_video_config_t *config);

/* -------------------------------------------------------------------- state */

typedef struct {
    /* What this build offers on this device. A viewport that was not known
     * at init can be ruled out on the first attempted session. */
    bool uplink_offered;               /* the camera can be sent */
    bool downlink_offered;             /* the viewer's video can be taken and shown */
    xiaotai_video_codec_t uplink_codec;
    xiaotai_video_codec_t downlink_codec;
    uint16_t uplink_width, uplink_height;   /* picture the uplink profile produces on the camera */
    /* The session. */
    uint32_t generation;               /* stream whose video session is open; 0: none */
    xiaotai_video_phase_t uplink;      /* phase of the camera uplink in that session */
    xiaotai_err_t uplink_error;        /* why it is XIAOTAI_VIDEO_ERROR, else XIAOTAI_OK */
    bool camera_enabled;               /* the user's switch for this session */
    bool receiving;                    /* the viewer was asked for its video */
    bool remote_ready;                 /* a picture of the viewer can be shown */
    bool remote_unsupported;           /* the viewer sends a format no decoder here handles */
} xiaotai_stream_video_status_t;

/* [any] */
void xiaotai_stream_video_status(xiaotai_stream_video_status_t *status);

/* ------------------------------------------------------------------ actions */

/* [any] The user's camera switch for the stream of this generation: off stops
 * sending and releases the camera, on restarts the uplink from a key frame.
 * Not remembered beyond the stream: every stream starts with the camera on.
 * Queued to the owner task: XIAOTAI_OK means accepted. XIAOTAI_ERR_INVALID_ARG
 * for generation 0; XIAOTAI_ERR_NOT_SUPPORTED when no uplink is offered;
 * XIAOTAI_ERR_NO_MEM when the owner's queue is full. A generation that is not
 * the open session when the request is reached is ignored and counted. */
xiaotai_err_t xiaotai_stream_video_set_camera(uint32_t generation, bool enabled);

/* ------------------------------------------------------------------- events */

/* The camera uplink of the stream changed phase. args.value: the new
 * xiaotai_video_phase_t; data: the video engine's xiaotai_video_event_t. */
#define XIAOTAI_EVENT_STREAM_VIDEO_UPLINK "stream_video.uplink"
/* args.value 1: the first picture of the viewer is ready to be shown, 0: the
 * viewer's video ended (with the stream at the latest). data: the generation
 * (uint32_t), which is what the video source is installed for (by this module
 * when the UI engine is selected, before the event is emitted; it is cleared
 * before the event with 0). */
#define XIAOTAI_EVENT_STREAM_VIDEO_REMOTE "stream_video.remote"
/* A new unsupported format, or its recovery on the same stream. Both use
 * args.value for the generation. */
#define XIAOTAI_EVENT_STREAM_VIDEO_UNSUPPORTED "stream_video.unsupported"
#define XIAOTAI_EVENT_STREAM_VIDEO_FORMAT_RECOVERED "stream_video.format_recovered"

/* ------------------------------------------------------------------ intents */

/* stream_video.camera: the camera switch of the stream that is active when
 * the intent is handled. args.value: 1 on, 0 off. */
#define XIAOTAI_INTENT_STREAM_VIDEO_CAMERA "stream_video.camera"

/* -------------------------------------------------------------- diagnostics */

/* Counters since boot. */
typedef struct {
    uint32_t sessions;             /* video sessions opened */
    uint32_t session_failures;     /* xiaotai_video_open refused */
    uint32_t uplink_errors;        /* uplink phases that ended in XIAOTAI_VIDEO_ERROR */
    uint32_t receive_requests;     /* times the viewer was asked for its video */
    uint32_t receive_failures;     /* asking failed; retried every second while the stream is active */
    uint32_t remote_ready;         /* streams whose viewer picture became showable */
    uint32_t remote_unsupported;
    uint32_t camera_switches;
    uint32_t stale;                /* requests and engine events for a generation that was not the session */
} xiaotai_stream_video_stats_t;

/* [any] */
void xiaotai_stream_video_stats(xiaotai_stream_video_stats_t *stats);

/* ---------------------------------------------------------------- lifecycle */

/* Module lifecycle (called by the framework through the generated tables).
 * init: reads the video engine's capabilities, declares the uplink profile,
 * the downlink and the viewport it will use, declares its send buffer to the
 * transport, fills its two contribution records (static storage: the link's
 * video part with send, send_format, peer_streams REFUSE, receive,
 * receive_request and one receive format; the scene part's writer, which
 * writes the codecs, rotation, mirrors, aspect ratio and fit, never the
 * stream numbers or "no_video": those are the stream module's) and
 * attaches to the stream activity with them, registers the intent and the
 * diagnostics section "stream_video" and subscribes to the video engine's
 * events. On a device where neither direction can be offered it declares and
 * contributes nothing, does not attach and stays idle: init still succeeds.
 * The module has no start step: the stream module collects the
 * contributions in its own start. */
xiaotai_err_t xiaotai_module_stream_video_init(void);
/* Host tests: forget configuration, state and counters. */
void xiaotai_stream_video_reset(void);

#ifdef __cplusplus
}
#endif
