// SPDX-License-Identifier: MIT
#pragma once
/* Base module "video_call": video on calls.
 *
 * The module is a session extension of the activity "call" (an attachment:
 * begin, end, session and contributions in xiaotai/framework.h). For the
 * host scene of every call module the product selects (device calls "call",
 * WeChat calls "voip"; at least one of them, the manifest's "one_of") it
 * contributes the video part of the call's link (XIAOTAI_LINK_SLOT_VIDEO)
 * and the scene's video members of the device profile
 * (XIAOTAI_PLATFORM_SLOT_SCENE); the call modules have no video API. When
 * the activity's generation becomes a link it asks each selected call module
 * (by its status) whether that link is its ACTIVE call with video, and if so
 * runs the video session on it until the generation returns to 0 or the
 * activity ends; it subscribes to no call module's events. The session
 * covers the camera uplink, the peer's picture, the subscription to the
 * peer's stream with its retries, and, when the product has the UI engine
 * and the board a display, the UI engine's built-in video source on the
 * display (xiaotai_ui_video_show / _clear). Without them the device only
 * sends; without a camera it only shows. The call itself (dialling, accepting, audio, ending) stays with the
 * call module; the engine (xiaotai/video.h) encodes, decodes and paces.
 *
 * What is sent and expected depends on who the peer is, never on the board:
 * another device decodes in software and gets and sends the compact H.264
 * picture, or JPEG where one of the two has no H.264 encoder or decoder (a
 * device that decodes both shows whichever arrives); a phone gets the camera's
 * own picture and sends JPEG.
 *
 * A camera, encoder or decoder failure never ends the call: the call goes on
 * with audio and the failure is reported.
 *
 * Tags as in xiaotai/framework.h: [init], [owner], [any]. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"
#include "xiaotai/video_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------- configuration */

/* How a scene's peer picture is laid out on the display. Passed with every
 * video session (the engine's xiaotai_video_placement_t); the display's
 * viewport itself is declared plain, so it equals what any other module
 * declares for the same display. Applied in this order: rotation,
 * orientation match, fit. A zeroed struct is valid. */
typedef struct {
    xiaotai_video_fit_t fit;       /* default XIAOTAI_VIDEO_FIT_CONTAIN */
    /* Quarter turns clockwise for every picture of this scene's peer (a
     * phone that sends its sensor's picture sideways for this display).
     * Default XIAOTAI_VIDEO_ROTATE_0. */
    xiaotai_video_rotation_t rotation;
    /* After the rotation, turn a portrait picture on a landscape area (or
     * the reverse) by a further quarter before fitting it. */
    bool match_orientation;
} xiaotai_video_call_layout_t;

/* Most entries of the scene table. */
#define XIAOTAI_VIDEO_CALL_SCENES_MAX 4U

/* What one host scene's video sessions and profile members are, beyond the
 * scene's protocol facts (which the module keeps per call module: what the
 * peer decodes and sends, the profile's member set and spelling). Product
 * facts the service was tuned with, never facts of the parts. A zeroed
 * entry is valid: it is what a scene the table does not name gets. */
typedef struct {
    /* The host scene: a call module's scene (XIAOTAI_CALL_SCENE "call",
     * XIAOTAI_WECHAT_SCENE "voip"). Static storage. NULL ends the table. */
    const char *scene;
    xiaotai_video_call_layout_t layout;   /* the peer's picture on the display */
    /* Picture budget when the scene's peer sends JPEG
     * (XIAOTAI_VIDEO_DOWNLINK_JPEG, xiaotai/video_profiles.h): either side at
     * most the larger number, the product of both sides rounded up to 16 at
     * most width x height, in either orientation. Never the display's size.
     * Both 0: 640 x 480. Ignored for a peer that sends H.264. */
    uint16_t picture_width, picture_height;
    /* Reported as camera_rotation: the turn, in degrees (0, 90, 180, 270),
     * the receiver adds to this device's pictures. Never taken from the
     * camera descriptor: the engine already sends upright pictures. Default 0. */
    uint16_t camera_rotation;
    /* Reported as aspect_ratio, written as it stands: a JSON string such as
     * "\"3:4\"" or a JSON number such as "1.7066666667". Static storage.
     * NULL: the uplink picture's ratio from xiaotai_video_resolve, as a
     * string "w:h" in lowest terms. */
    const char *aspect_ratio_json;
    /* Reported as video_res_mode by a scene whose member set has it ("voip"):
     * NULL "auto", or a word such as "fit_screen". Static storage. */
    const char *video_res_mode;
    /* The peer can send its picture on its own valid stream number, distinct
     * from the one requested by subscription. The scene's peer codec and
     * session checks still apply. Default false: receive on the requested stream. */
    bool accept_peer_video_stream;
    /* Calls of this scene stay voice calls on this product, whatever its
     * parts can do (no room for a phone's picture, for example). */
    bool voice_only;
} xiaotai_video_call_scene_t;

/* Product configuration. A zeroed struct is valid. */
typedef struct {
    /* Display part that shows the peer; default 0. Without that part, or
     * without the UI engine in the product, the device only sends video and
     * the profile reports no video downlink. */
    uint8_t display;
    uint8_t camera;                        /* camera part; default 0 */
    /* Per host scene; ends at the first entry without a scene. */
    xiaotai_video_call_scene_t scenes[XIAOTAI_VIDEO_CALL_SCENES_MAX];
} xiaotai_video_call_config_t;

/* [glue] Before xiaotai_framework_start(), by the product glue; copied (the
 * strings are kept by pointer). NULL or never called: all defaults.
 * XIAOTAI_ERR_INVALID_ARG for an empty scene name, a scene named twice, a
 * camera_rotation other than 0, 90, 180 or 270, or a layout value out of
 * range; XIAOTAI_ERR_INVALID_STATE after the module's init. */
xiaotai_err_t xiaotai_video_call_configure(const xiaotai_video_call_config_t *config);

/* ------------------------------------------------------------------- state */

typedef struct {
    bool available;        /* this device can send or show video on calls */
    bool active;           /* a video session runs on a call's link */
    uint32_t link;         /* its generation (the call's link); 0: none */
    bool camera;           /* this device's camera is part of the call */
    bool camera_enabled;   /* the user's switch for this call; true at the start of every call with a camera */
    bool sending;          /* the camera uplink runs */
    bool receiving;        /* the peer was asked for its video and frames are accepted */
    bool remote;           /* a picture of the peer can be shown */
    bool unsupported;      /* the peer sends a format no decoder here handles */
    xiaotai_err_t error;   /* last failure of the uplink or the subscription; XIAOTAI_OK */
    uint32_t sessions;     /* video sessions since boot */
    uint32_t subscribe_retries;
    uint32_t stale_calls;  /* camera switches that named another link */
} xiaotai_video_call_status_t;

/* [owner] */
void xiaotai_video_call_status(xiaotai_video_call_status_t *status);

/* [owner] Whether calls of this scene (XIAOTAI_CALL_SCENE, XIAOTAI_WECHAT_SCENE)
 * can carry video here, in at least one direction. */
bool xiaotai_video_call_offers(const char *scene);
/* [owner] ... and whether this device's camera is part of it. */
bool xiaotai_video_call_sends(const char *scene);
/* [owner] ... and whether a picture of the peer can be shown here. */
bool xiaotai_video_call_receives(const char *scene);

/* [owner] The user's camera switch during a video call. `link` names the
 * video session (status.link, the call's link); 0 means the current one, for
 * sources that hold no snapshot. Off: sending stops at once and the camera
 * is released; the peer's picture stays. On: the uplink restarts with a key
 * frame. Not remembered: every call starts with the camera on.
 * XIAOTAI_ERR_INVALID_STATE unless a video session is active, and for
 * another link than the active one (counted in stale_calls), so a tap on the
 * page of an earlier call never switches the camera of the next one. */
xiaotai_err_t xiaotai_video_call_set_camera(uint32_t link, bool enabled);

/* -------------------------------------------------- events and intents */

/* Events on the framework bus; data is a xiaotai_video_call_status_t.
 *
 * "video_call.state": the status changed. args.value: 1 while a video
 * session is active, 0 otherwise. */
#define XIAOTAI_EVENT_VIDEO_CALL_STATE "video_call.state"
/* "video_call.remote": args.value 1: the first picture of the peer is ready
 * and the display's video region shows it; 0: it ended. */
#define XIAOTAI_EVENT_VIDEO_CALL_REMOTE "video_call.remote"
/* "video_call.unsupported": args.value 1 on a newly unsupported format,
 * 0 when the current peer switches back to a supported format. */
#define XIAOTAI_EVENT_VIDEO_CALL_UNSUPPORTED "video_call.unsupported"

/* Intents the module handles; args.value names the video session (its link,
 * 0: the current one), as xiaotai_video_call_set_camera.
 *   "video_call.camera_on"      camera on
 *   "video_call.camera_off"     camera off
 *   "video_call.camera_toggle"  the opposite of status.camera_enabled */
#define XIAOTAI_INTENT_VIDEO_CALL_CAMERA_ON "video_call.camera_on"
#define XIAOTAI_INTENT_VIDEO_CALL_CAMERA_OFF "video_call.camera_off"
#define XIAOTAI_INTENT_VIDEO_CALL_CAMERA_TOGGLE "video_call.camera_toggle"

/* Commands, as verbs of AT+CALL: the line +CALL-VIDEO: video= camera= ...,
 * VIDEO <n> (dial the contact at index n with video), CAMERA 0|1.
 * Diagnostics section "video_call". Event-log source "video_call". */

/* ---------------------------------------------------------------- lifecycle */

/* Module lifecycle (called by the framework through the generated tables).
 * init: reads the video engine's capabilities, declares the profiles, the
 * downlinks and the display's plain viewport
 * (xiaotai_video_viewport_for_display; downlinks and viewport only with a
 * display part and the UI engine) and the send buffer its key frames need,
 * attaches to the activity "call" with a link video part and a scene part
 * for the scene of each call module the product selected, subscribes to the
 * video engine's events, registers the intents and the command verbs. A
 * device that can neither encode nor decode initializes successfully with
 * available false and contributes nothing. */
xiaotai_err_t xiaotai_module_video_call_init(void);
/* Host tests: close the session and forget configuration and state. */
void xiaotai_video_call_reset(void);

#ifdef __cplusplus
}
#endif
