// SPDX-License-Identifier: MIT
#pragma once
/* Video engine (optional): the camera uplink, the picture a peer sends, and the
 * governor that keeps both inside what the link and the device can carry.
 *
 * Only products with a camera select it, through a base module that needs
 * video (video call, web stream video, AI vision). The engine knows no module,
 * link kind, stream number or screen. A module opens a session and hands over
 *   - an uplink profile: what to encode from the camera,
 *   - a downlink description: what the peer sends,
 *   - a link: the functions that carry frames for this session
 *     (xiaotai/video_link.h has the one for transport links),
 * and whoever draws the screen declares a viewport and fetches finished frames.
 * Encoders, decoders and picture transforms are backends chosen by score
 * (xiaotai/video_backend.h); a build without a decoder has no downlink, so a
 * product that only sends JPEG carries none of the rest.
 *
 * Sessions are keyed by the link's generation (0 is invalid). One session
 * exists at a time: opening another replaces it. Every call and every frame
 * carries the generation; whatever belongs to another generation is dropped
 * and counted.
 *
 * Who may call: [init], [owner], [any] and [rt] as defined in
 * xiaotai/framework.h. The requirement functions admit fewer callers and say
 * so at the function: "[init], engines excluded" (what sessions will use is
 * declared by modules and applications). Plus the engine's own:
 *   [presenter]  the one task that draws the screen (the UI task); as [rt]
 * No function here may be called from an interrupt handler.
 *
 * The engine calls a session's link functions on its own tasks: [session] its
 * session task, [uplink] its camera task. Handlers of the events below run on
 * the owner task, like every framework event handler.
 *
 * Results: XIAOTAI_OK; XIAOTAI_ERR_INVALID_ARG (a parameter breaks the rules
 * stated at the function); XIAOTAI_ERR_INVALID_STATE (too early, too late, or
 * not the open session); XIAOTAI_ERR_NOT_FOUND (no such part);
 * XIAOTAI_ERR_NOT_SUPPORTED (no backend, or not declared before start);
 * XIAOTAI_ERR_NO_MEM (a table is full, or could not be allocated). */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"
#include "xiaotai/video_config.h"
#include "xiaotai/video_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------- uplink profile */

/* What a session encodes from the camera. Chosen by the module for its peer (a
 * peer that decodes in software gets a small picture; a phone gets the sensor's
 * picture); never derived from a board. xiaotai/video_profiles.h has the tuned
 * profiles in use.
 *
 * Fields a codec does not use are 0. H.264 values are normalized when the
 * profile is declared: fps 5..30, bitrate 200 kbit/s..12 Mbit/s, QP 10..51, at
 * least 320 x 240, even dimensions. JPEG accepts fps 1..30. */
typedef struct {
    xiaotai_video_codec_t codec;
    /* Encoded picture.
     * Both non-zero: exactly width x height. A camera mode of that size is used
     * as it is; otherwise the camera's native picture is cropped around its
     * centre to this aspect ratio and scaled (a transform that scales only in
     * fixed steps crops more instead of distorting).
     * Both zero: the camera's whole native picture, scaled down by the largest
     * exact factor that keeps it within max_pixels (0: no limit). */
    uint16_t width, height;
    uint32_t max_pixels;
    /* A smaller size the session drops to once, after quality reached
     * min_quality and pressure persists; both zero: never. Used by JPEG. The
     * camera is closed and reopened for it, never resized while it runs. */
    uint16_t fallback_width, fallback_height;
    /* Turn the picture by the camera's mounting rotation so the peer sees it
     * upright. A fixed width x height is the size sent, after the turn; a
     * derived size has its width and height exchanged by a quarter turn. */
    bool upright;
    uint8_t fps;
    /* H.264: the bitrate at full quality. It is also the top of the range
     * negotiated with the link (xiaotai/video_governor.h). */
    uint32_t bitrate_bps;
    uint8_t min_qp, max_qp;
    /* H.264: seconds between scheduled key frames (the frame count is capped
     * at 255). Requests, a new viewer and recovery after a failed send still
     * force one at once. */
    uint8_t key_interval_s;
    /* JPEG: quality at session start, and the floor it steps down to (by 5 per
     * 5 s evaluation window) while the link is under pressure. Quality does
     * not rise at the same resolution. A product can restore this initial
     * quality once when falling back to a smaller resolution. */
    uint8_t quality, min_quality;
    /* Largest frame handed to the link. JPEG: also the encoder's output buffer;
     * a larger result fails the session. H.264: a larger frame that is not a
     * key frame is dropped and the next frame becomes a key frame (half this
     * limit during the first 2.5 s of a stream). */
    uint32_t max_frame_bytes;
    /* Frames are skipped, before encoding and again before sending, while the
     * link holds at least this many unsent bytes, so video never delays audio
     * on a shared link. 0: only the link's own limit applies. */
    uint32_t backlog_limit_bytes;
    /* A captured picture older than this when it would be encoded or sent is
     * dropped and counted as stale; 0: no limit. */
    uint16_t max_age_ms;
    /* JPEG: pressure events (frames skipped for congestion, failed or late) in
     * one 5 s window that trigger a step down; 0: never step down. */
    uint8_t pressure_limit;
    /* After a stale picture, take the next one at once instead of waiting for
     * the next period (single stills at a low rate). */
    bool refetch_stale;
    /* When the uplink fails: retry after this many milliseconds while the
     * session stays open; 0: stay failed until another session opens. An
     * exhausted first-frame recovery ends the uplink regardless of this. */
    uint16_t retry_ms;
} xiaotai_video_profile_t;

/* ------------------------------------------------------ downlink description */

/* What the peer of a session sends. */
typedef struct {
    xiaotai_video_codec_t codec;
    /* Largest picture kept.
     * H.264: the decoded picture is cropped around its centre to max_width x
     * max_height, and a smaller one is a decode failure.
     * JPEG: a budget independent of orientation, because a peer may send
     * landscape or portrait pictures. A picture is accepted when neither side
     * exceeds the longer of max_width and max_height and its sides, each
     * rounded up to 16, multiply to at most max_width x max_height; any other
     * is refused and counted. The decode buffer holds exactly that budget, so
     * 640 x 480 accepts 640 x 480 and 480 x 640 alike. */
    uint16_t max_width, max_height;
    /* Frame rate the peer sends; paces buffered playout. 0: unknown, allowed
     * only when burst_ms is 0. */
    uint8_t fps;
    /* Largest encoded frame accepted; larger ones are dropped and counted. */
    uint32_t max_frame_bytes;
    /* Longest delivery stall (frames arriving late, then all at once) that is
     * ridden out by buffering instead of dropping to the next key frame; it
     * sizes the pool of encoded frames (fps * burst_ms / 1000 of them, at
     * least 4).
     * 0: every frame stands alone; only the newest is decoded and shown. */
    uint32_t burst_ms;
    /* Ask the peer for a key frame this often even while decoding is healthy:
     * a safety net on links where a lost frame goes unnoticed by the decoder.
     * 0: only while the decoder waits for a key frame. Never more often than
     * once a second, and never once the stream proved unsupported.
     * A codec whose every frame is a key frame (JPEG) has nothing to ask for:
     * its downlink never asks the peer, whatever this field, its drops, or the
     * link offer (such peers may not know the request at all). */
    uint16_t key_refresh_ms;
    /* Permit a supported H.264 SPS to recover a rejected stream without
     * reopening the link. While rejected we do not ask for key frames. */
    bool recover_h264_profile;
    /* Preserve the decoder's reference state on in-band SPS/PPS change;
     * invalidated header knowledge must still be supplied before a key. */
    bool preserve_h264_parameter_chain;
} xiaotai_video_downlink_t;

/* ----------------------------------------------------------------- viewport */

/* How a received picture is turned and placed in the area. Applied in this
 * order: the fixed rotation, then the orientation match, then the fit. */
typedef struct {
    xiaotai_video_fit_t fit;
    /* Quarter turns clockwise applied to every picture: for a peer that sends
     * its sensor's picture as it comes, sideways for this display. */
    xiaotai_video_rotation_t rotation;
    /* After the fixed rotation, turn a picture a further quarter clockwise
     * when it is portrait and the area landscape, or the reverse. */
    bool match_orientation;
} xiaotai_video_placement_t;

/* Where received pictures are drawn: an area of a display. Declared by whoever
 * presents them (a module, from the display part and the product's choices).
 * One viewport per display; one more display is one more declaration, and a
 * session names the display. Its placement is the default for sessions that
 * do not bring their own (xiaotai_video_session_t.placement).
 *
 * Finished frames are rendered at the area's size divided by the largest whole
 * number n (1..4) that divides both sides and still leaves room for the
 * downlink's largest picture (JPEG: max_width x max_height in either
 * orientation; n = 1 when none does); the presenter repeats each pixel n times
 * in both directions while painting. A small stream is thus never blown up in
 * memory, and a stream as large as the area is rendered once. */
typedef struct {
    uint8_t display;               /* index of the display part */
    /* The area, in display pixels (the logical frame, as xiaotai_video_area_t);
     * normally the whole display. */
    uint16_t width, height;
    /* What the presenter's target holds: XIAOTAI_VIDEO_PIXEL_RGB565,
     * _RGB565_SWAPPED or _RGB888. The engine's own software transform writes
     * all three, so a layout an accelerator cannot write still works. */
    xiaotai_video_pixel_t pixel;
    /* Default placement, as in xiaotai_video_placement_t. */
    xiaotai_video_fit_t fit;
    bool match_orientation;
    xiaotai_video_rotation_t rotation;
    /* Finished frames held for this viewport; 0: the engine's default (20 for
     * a buffered downlink, 3 otherwise). At least 3: one being written, one
     * ready, one shown. */
    uint8_t frames;
} xiaotai_video_viewport_t;

/* [any] Fills *viewport for a whole display part: its size, the pixel layout
 * its descriptor implies (RGB565 in the byte order XIAOTAI_DISPLAY_SWAP_BYTES
 * gives, or RGB888), fit CONTAIN, no turning, default frame count. The caller
 * then sets what the product chooses and declares it.
 *   XIAOTAI_ERR_NOT_FOUND      no display part at that index
 *   XIAOTAI_ERR_NOT_SUPPORTED  a display without colour pixels (monochrome) */
xiaotai_err_t xiaotai_video_viewport_for_display(size_t display, xiaotai_video_viewport_t *viewport);

/* ------------------------------------------------------------- requirements */

/* Capacities: XIAOTAI_VIDEO_PROFILES_MAX distinct uplink profiles and
 * XIAOTAI_VIDEO_VIEWPORTS_MAX viewports (one per display), build options of
 * xiaotai/video_config.h; each table is allocated from XIAOTAI_MEM_BULK by its
 * first declaration. Downlink descriptions are kept one per codec (the largest
 * of each field), so they need no capacity. */

/* [init], engines excluded. Declare what sessions will use. The engine
 * creates its tasks and reserves at its start whatever is large or needs
 * contiguous internal memory:
 * the resident memory of the cameras (see xiaotai_engine_video_start), one
 * encoder kept open for the declared profile whose encoder needs the most of
 * it, the scalers and scaler buffers of the declared profiles, and the frame
 * pools of every declared downlink and viewport. A session then never depends
 * on finding such memory later, and may only use what was declared. Only the
 * output buffer of a software encoder (the profile's max_frame_bytes,
 * XIAOTAI_MEM_BULK) is taken when its uplink opens and freed when it closes.
 * Declaring nothing costs nothing: no task, no buffer, no table. The structs
 * are copied; declaring an equal one twice is not an error.
 *
 * Viewports: a second declaration for a display that has one is accepted when
 * area (width, height), pixel and placement (fit, rotation, match_orientation)
 * are equal, and the larger frame count is kept; any other is refused, so the
 * outcome never depends on the order modules initialize in. Modules that lay
 * out different peers differently declare the display's plain viewport (as
 * xiaotai_video_viewport_for_display fills it) and pass their placement with
 * each session.
 *   XIAOTAI_ERR_INVALID_ARG    NULL, no codec, or values out of range
 *   XIAOTAI_ERR_INVALID_STATE  the engine has started; or a different viewport
 *                              is already declared for that display
 *   XIAOTAI_ERR_NO_MEM         the table is full, or could not be allocated */
xiaotai_err_t xiaotai_video_require_uplink(const xiaotai_video_profile_t *profile);
xiaotai_err_t xiaotai_video_require_downlink(const xiaotai_video_downlink_t *downlink);
xiaotai_err_t xiaotai_video_require_viewport(const xiaotai_video_viewport_t *viewport);

/* ------------------------------------------------------------- capabilities */

typedef struct {
    uint32_t uplink_codecs;    /* XIAOTAI_VIDEO_CODEC_BIT(): a camera exists and an encoder backend takes its pictures */
    uint32_t downlink_codecs;  /* a decoder backend exists (shown only on a declared viewport) */
    uint16_t camera_width, camera_height;      /* native picture of camera 0; 0: no camera */
    xiaotai_video_rotation_t camera_rotation;  /* its mounting rotation */
    bool camera_full_range;                    /* its pictures use the full 0..255 range */
} xiaotai_video_capabilities_t;

/* [any] Valid once the engine is initialized, so modules read it in init() to
 * decide what they offer and what the device profile reports. */
void xiaotai_video_capabilities(xiaotai_video_capabilities_t *capabilities);

/* Cameras that can be unplugged. No camera part of this version is removable
 * (xiaotai/hal.h, "Parts are hardware fixed"); this fixes what the engine will
 * do when one is, so that modules and sessions written now need no change:
 *   - an inserted camera is taken up only while no session has an uplink on
 *     it: the engine then reads its descriptor again, so the capabilities,
 *     xiaotai_video_resolve() and the modes opened follow the new device, and
 *     announces the change with an event of its own (added with that
 *     version) on which modules that offer video read the capabilities again;
 *   - a camera removed while a session's uplink uses it ends that uplink as a
 *     failure of the part: XIAOTAI_EVENT_VIDEO_UPLINK reports
 *     XIAOTAI_VIDEO_ERROR with XIAOTAI_ERR_NOT_FOUND, the downlink and the
 *     rest of the session go on, and the uplink is not tried again (whatever
 *     the profile's retry_ms) before that session closes; the next session
 *     uses the device present then. */

/* [any] The picture size a profile produces on a camera, after cropping,
 * scaling and the upright turn. Valid once the engine is initialized.
 *   XIAOTAI_ERR_NOT_FOUND      no camera at that index
 *   XIAOTAI_ERR_NOT_SUPPORTED  no encoder or transform backend can produce it,
 *                              or it is larger than the camera's picture */
xiaotai_err_t xiaotai_video_resolve(const xiaotai_video_profile_t *profile, size_t camera,
                                    uint16_t *width, uint16_t *height);

/* --------------------------------------------------------------------- link */

/* The functions that carry video for a session. xiaotai/video_link.h provides
 * the link for transport links; another carrier is one more object of this
 * type. The object and ctx have static lifetime: the engine may still call
 * them, with a stale generation, while a closed session winds down. Every
 * function receives the session's generation and must treat a generation that
 * is no longer its current connection as "not ready". They are called on
 * engine tasks, never on the owner task and never while the engine holds a
 * lock; each returns within a few milliseconds, except send, which may take as
 * long as the carrier's own send. ready, backlog, send and
 * take_key_frame_request are called only on the camera task, the one task
 * that encodes and sends a session's frames (a carrier's "sending task", the
 * [uplink] of xiaotai/link.h); never on a capture, playout or receive path.
 * Members marked optional may be NULL. */
typedef struct xiaotai_video_link {
    /* [uplink] The peer can take video now: connected, this generation, and
     * subscribed or permitted as the carrier requires. Checked before every
     * costly step (power, open, encode, send). */
    bool (*ready)(void *ctx, uint32_t generation);

    /* [uplink] Optional. Bytes handed over and not yet sent, compared with the
     * profile's backlog_limit_bytes; 0 for a stale generation. */
    size_t (*backlog)(void *ctx, uint32_t generation);

    /* [uplink] Send one encoded frame; packet->data is valid during the call.
     * backlog_limit_bytes is the profile's (0: none): the carrier refuses with
     * XIAOTAI_ERR_BUSY at or above it.
     *   XIAOTAI_OK                 taken by the link
     *   XIAOTAI_ERR_INVALID_STATE  not ready or another generation: dropped
     *                              quietly, not a sign of congestion
     *   XIAOTAI_ERR_BUSY           congested: not taken
     *   anything else              the send failed
     * After BUSY or a failure a codec with a reference chain resumes from a key
     * frame and the camera task takes no picture for the backpressure hold. */
    xiaotai_err_t (*send)(void *ctx, uint32_t generation, const xiaotai_video_packet_t *packet,
                          uint32_t backlog_limit_bytes);

    /* [uplink] Optional. True once after the peer asked this side for a key
     * frame or started viewing; requests since the last call merge into one.
     * Handled as XIAOTAI_VIDEO_KEY_PEER_REQUEST. */
    bool (*take_key_frame_request)(void *ctx, uint32_t generation);

    /* [session] Optional. Ask the peer for a key frame; NULL when the peer
     * cannot be asked. */
    xiaotai_err_t (*request_key_frame)(void *ctx, uint32_t generation);

    /* [session] Optional, as a pair. Offer the link the bitrate range of the
     * uplink (0 < min < start < max). An error means the link gives no
     * feedback: the profile's bitrate is then used unchanged. */
    xiaotai_err_t (*set_bitrate_range)(void *ctx, uint32_t generation, uint32_t min_bps,
                                       uint32_t start_bps, uint32_t max_bps);
    /* [session] True and the newest target when the link has one the engine has
     * not taken yet (several suggestions collapse into the latest). */
    bool (*take_bitrate_target)(void *ctx, uint32_t generation, uint32_t *target_bps);

    void *ctx;
} xiaotai_video_link_t;

/* ----------------------------------------------------------------- sessions */

typedef struct {
    uint32_t generation;                       /* the link's generation; 0 is invalid */
    const xiaotai_video_profile_t *uplink;     /* NULL: the camera is not used */
    const xiaotai_video_downlink_t *downlink;  /* NULL: nothing is received */
    const xiaotai_video_link_t *link;          /* static lifetime; ready and send are required */
    uint8_t camera;                            /* index of the camera part; 0 for the first */
    uint8_t display;                           /* display whose declared viewport shows the downlink */
    /* How this peer's picture is turned and placed; NULL: the viewport's
     * default. Lets one viewport per display serve peers that send pictures
     * differently (a phone's sideways sensor picture, a device's upright one).
     * Copied; it changes the plan, not the area, the pixel layout or the
     * memory reserved. */
    const xiaotai_video_placement_t *placement;
} xiaotai_video_session_t;

/* [owner] Make this the video session. Returns at once; the engine's session
 * task does the work and reports through the events and xiaotai_video_status().
 * A session that is open under another generation is stopped first (uplink,
 * then downlink; the engine waits for its camera task to let go of the camera
 * before it continues), then this one starts: downlink, then, once the session
 * is in effect, the bitrate range, the uplink and a first key frame. The camera
 * starts enabled in every session. A start that fails is tried again every
 * 500 ms while the session stays open (the uplink according to its profile's
 * retry_ms). The profile, downlink and placement structs are copied. Opening
 * the generation that is already open with the same description changes
 * nothing.
 *   XIAOTAI_ERR_INVALID_ARG    generation 0, neither uplink nor downlink, no
 *                              link, a link without ready and send, or a
 *                              placement with values out of range
 *   XIAOTAI_ERR_INVALID_STATE  the engine has not started, or this generation
 *                              is open with another description (close it first)
 *   XIAOTAI_ERR_NOT_FOUND      uplink without a camera at that index
 *   XIAOTAI_ERR_NOT_SUPPORTED  a profile or downlink that was not declared, no
 *                              backend for its codec, or a downlink without a
 *                              declared viewport on that display */
xiaotai_err_t xiaotai_video_open(const xiaotai_video_session_t *session);

/* [owner] End the session of this generation: sending stops at once, the camera
 * is closed before it is powered down, and the frame pools stay reserved. A
 * generation that is not the open session is ignored and counted. */
void xiaotai_video_close(uint32_t generation);

/* [owner] The user's camera switch during a session with an uplink. Off: sending
 * stops at once and the camera is released; on: the uplink restarts, once the
 * camera task has let go of the camera, and begins with a key frame. The
 * peer's picture is not affected, and the switch is not remembered beyond the
 * session.
 *   XIAOTAI_ERR_INVALID_STATE  not the open session, or it has no uplink */
xiaotai_err_t xiaotai_video_set_camera_enabled(uint32_t generation, bool enabled);

typedef enum {
    /* The peer lost a frame. Requests within 2 s of an honoured one merge into it. */
    XIAOTAI_VIDEO_KEY_PEER_REQUEST = 0,
    /* A viewer just joined and has no picture yet: always honoured. */
    XIAOTAI_VIDEO_KEY_NEW_VIEWER,
} xiaotai_video_key_reason_t;

/* [rt] The uplink's next frame should be a key frame; for carriers that report
 * requests by a call instead of the link's take_key_frame_request. No effect
 * on codecs whose every frame is one, or when generation is not the session in
 * effect. */
void xiaotai_video_request_key_frame(uint32_t generation, xiaotai_video_key_reason_t reason);

/* [rt] A frame from the peer; one producer at a time. Copied once into the
 * engine's pool before this returns; packet->data is not kept. False when
 * dropped: another generation, no downlink or another codec, empty or larger
 * than the declared frame, or no free slot. Every drop is counted by reason.
 * A drop on a codec with a reference chain (H.264) makes the engine ask the
 * peer for a key frame; one whose every frame is a key frame (JPEG) asks
 * nothing: the next frame stands alone. */
bool xiaotai_video_push(uint32_t generation, const xiaotai_video_packet_t *packet);

/* ------------------------------------------------------------------- events */

/* Emitted on the framework event bus; args.value as described, data is a
 * xiaotai_video_event_t. */
#define XIAOTAI_EVENT_VIDEO_UPLINK "video.uplink"            /* the camera uplink changed phase; value: xiaotai_video_phase_t */
#define XIAOTAI_EVENT_VIDEO_REMOTE "video.remote"            /* value 1: the first picture of the peer is ready; 0: its video ended */
#define XIAOTAI_EVENT_VIDEO_UNSUPPORTED "video.unsupported"  /* the peer sends a format no decoder here handles */
#define XIAOTAI_EVENT_VIDEO_FORMAT_RECOVERED "video.format_recovered" /* a supported SPS replaced a rejected one */

typedef enum {
    XIAOTAI_VIDEO_OFF = 0,
    XIAOTAI_VIDEO_STARTING,
    XIAOTAI_VIDEO_RUNNING,
    XIAOTAI_VIDEO_STOPPING,
    XIAOTAI_VIDEO_ERROR,     /* failed; see error. Left by a retry or another session */
} xiaotai_video_phase_t;

typedef struct {
    uint32_t generation;
    xiaotai_video_phase_t phase;   /* XIAOTAI_EVENT_VIDEO_UPLINK; otherwise XIAOTAI_VIDEO_OFF */
    xiaotai_err_t error;           /* why the phase is XIAOTAI_VIDEO_ERROR, else XIAOTAI_OK */
} xiaotai_video_event_t;

/* ------------------------------------------------------------- presentation */

/* A finished picture of the peer, laid out for the session's viewport.
 * pixels is cache-line aligned BULK memory owned by the engine, rows tightly
 * packed. */
typedef struct {
    uint32_t generation;
    uint32_t sequence;             /* counts the frames handed out in this generation */
    xiaotai_video_pixel_t pixel;   /* the viewport's */
    uint16_t width, height;        /* the buffer: the viewport's area divided by scale */
    uint8_t scale;                 /* each pixel covers scale x scale display pixels (1..4) */
    xiaotai_video_rect_t picture;  /* where the picture lies in the buffer; everything else is black */
    const void *pixels;
    size_t size;                   /* bytes at pixels */
} xiaotai_video_frame_t;

/* [presenter] Take the frame to show now. True: a newer frame is due; it is
 * described in *frame (which may be NULL) and pinned (never overwritten or
 * reused) until the next call that returns true or
 * xiaotai_video_frame_release(), and the frame pinned before it is free
 * again. False: nothing new is due; a frame pinned earlier stays pinned and
 * valid, also after its session ended. Normally the newest frame is returned
 * and older ones are skipped; after a confirmed delivery stall of a buffered
 * downlink, frames are handed out in order at the sender's frame rate instead.
 * Call it at least every 10 ms while the picture is visible. */
bool xiaotai_video_frame_acquire(xiaotai_video_frame_t *frame);

/* [presenter] Like acquire, restricted to the source installed for generation.
 * A mismatched or inactive generation consumes no frame and leaves any pin
 * unchanged. Use this when replacing video sources can race an active draw. */
bool xiaotai_video_frame_acquire_for(uint32_t generation, xiaotai_video_frame_t *frame);

/* Part of a display's frame a presenter may write: pixels addresses the pixel
 * at (x, y) of the display, rows are stride bytes apart. x, y, width and
 * height are display pixels, like xiaotai_video_area_t (the logical frame the
 * UI draws in, never the panel's native frame). */
typedef struct {
    uint8_t *pixels;
    uint16_t x, y, width, height;
    uint32_t stride;
    xiaotai_video_pixel_t pixel;
} xiaotai_video_surface_t;

/* [presenter] Copy the pinned frame into surface. The frame, enlarged by its
 * scale, is centred in area (the viewport's place on the display, in display
 * pixels); only the part that falls into both area and surface is written, so
 * it may be called once per strip of a partial refresh. A plain row copy, or
 * pixel repetition for scale > 1: never a resampling. False, with nothing
 * written, when no frame is pinned, the surface's pixel layout is not the
 * frame's, or nothing overlaps. */
bool xiaotai_video_frame_paint(const xiaotai_video_surface_t *surface, const xiaotai_video_area_t *area);

/* [presenter] Where xiaotai_video_frame_paint() with this area puts the
 * picture of the pinned frame: the frame's picture rectangle, enlarged by its
 * scale and centred exactly as paint centres the frame, clipped to area, in
 * display pixels. The frame's black margins are the same in every frame of
 * one layout, so a presenter that refreshes only this rectangle and the one of
 * its previous refresh (a video source's placement(), xiaotai/ui.h) shows the
 * same picture as one that refreshes the whole area. False, *picture
 * unchanged, when no frame is pinned or the picture does not overlap area. */
bool xiaotai_video_frame_placement(const xiaotai_video_area_t *area, xiaotai_video_area_t *picture);

/* [presenter] Unpin the frame (the picture is hidden or the screen is
 * rebuilt). Harmless when none is pinned. Also allowed on the owner task once
 * the presenter is known to have stopped calling the two functions above (its
 * video source was removed), so a module can let go of the last frame. */
void xiaotai_video_frame_release(void);

/* [any] Generation of the session whose peer picture can be shown; 0: none. */
uint32_t xiaotai_video_remote_generation(void);

/* -------------------------------------------------------------- diagnostics */

typedef struct {
    xiaotai_video_phase_t phase;
    xiaotai_err_t error;
    xiaotai_video_codec_t codec;
    bool camera_enabled;
    uint16_t width, height;     /* encoded picture now */
    uint8_t quality;            /* JPEG quality now */
    uint32_t bitrate_bps;       /* H.264 target now */
    uint32_t fps_x10;           /* frames sent per second over the last window, times 10 */
    uint32_t captured;          /* pictures taken from the camera */
    uint32_t sent;              /* frames the link took */
    uint32_t key_frames;        /* of those, key frames */
    uint32_t key_requests;      /* key frames asked for by the peer (honoured or merged) */
    uint32_t stale;             /* dropped: too old, or the session was no longer admitted */
    uint32_t congested;         /* skipped: the link's backlog was at the limit, or it answered busy */
    uint32_t late;              /* a period that took longer than the frame interval */
    uint32_t oversize;          /* dropped: larger than the profile's frame limit */
    /* capture_failed: the camera refused a frame, or gave none for the
     * no-frame limit (camera borrows never wait, so the engine times the
     * gap): JPEG 1.5 s, after which the uplink fails; H.264 3 s, counted once
     * per gap, then a 200 ms pause and capture goes on. The first-frame gap
     * instead allows one camera reopen, unless disabled by the product. */
    uint32_t capture_failed, encode_failed, send_failed;
    uint32_t backpressure;      /* times capture paused after a failed send */
    uint32_t encode_max_us, send_max_us, age_max_ms;
    /* A recovery takes the original camera mode through close/off/on/open.
     * Recovered counts the first camera loan, not a packet sent to the peer.
     * Cancellation does not count as exhaustion. */
    uint32_t first_frame_reopens, first_frame_recovered, first_frame_exhausted;
} xiaotai_video_uplink_status_t;

typedef struct {
    xiaotai_video_phase_t phase;
    xiaotai_err_t error;
    xiaotai_video_codec_t codec;
    bool waiting_key_frame;     /* frames are discarded until a key frame arrives */
    bool unsupported;           /* the peer's stream is a format no decoder here handles */
    bool quarantined;           /* a decode call has not returned; nothing is accepted until it does */
    bool buffering;             /* buffered playout is in force (after a delivery stall) */
    uint16_t source_width, source_height;
    uint64_t received_bytes;
    uint32_t received;          /* frames accepted by xiaotai_video_push */
    uint32_t stale;             /* dropped: another generation */
    uint32_t rejected;          /* dropped: wrong codec, empty or too large */
    uint32_t overflow;          /* dropped: no free slot */
    uint32_t discarded;         /* dropped while waiting for a key frame, or superseded before decoding */
    uint32_t decoded, decode_failed;
    uint32_t converted, convert_failed;
    uint32_t presented;         /* frames handed to the presenter */
    uint32_t skipped;           /* finished frames replaced by a newer one before being shown */
    uint32_t discontinuities;   /* times the decoder was restarted from a key frame */
    uint32_t key_requests;      /* key frames asked of the peer */
    uint32_t queue_depth;       /* encoded frames waiting now */
    uint32_t playout_depth;     /* finished frames waiting now */
} xiaotai_video_downlink_status_t;

typedef struct {
    bool started;               /* the engine reserved what was declared and accepts sessions */
    uint32_t requested;         /* generation of the open session; 0: none */
    uint32_t generation;        /* generation in effect (started); 0 while switching */
    uint32_t ignored;           /* calls that named a generation that was not the session */
    xiaotai_video_uplink_status_t uplink;
    xiaotai_video_downlink_status_t downlink;
} xiaotai_video_status_t;

/* [any] A snapshot, read field by field; counters restart with each session
 * and stay readable after it closed. The engine also registers the diagnostics
 * provider "video" and, through xiaotai_at_verbs_register, the AT+VIDEO verbs
 * (status, CAPTURE ..., PROBE; the camera self-test is AT+VIDEO PROBE, there
 * is no AT+CAMERA). Their output lines are fixed by the engine's
 * specification. */
void xiaotai_video_status(xiaotai_video_status_t *status);

/* ---------------------------------------------------------------- lifecycle */

typedef struct {
    /* JPEG only. On the one-way switch to the fallback resolution, start
     * that resolution at the profile's initial quality. Same-resolution
     * restarts still retain the current quality; default false. */
    bool restore_initial_quality_on_resolution_reopen;
    /* Preserve adaptive presentation for independent frames when a product
     * already uses it. Default false; JPEG still decodes only the newest
     * compressed input and never requests a peer key frame. */
    bool buffer_independent_frames;
    /* Opt out of the one camera reopen on first-frame timeout. Default false. */
    bool disable_first_frame_recovery;
} xiaotai_video_config_t;

/* [glue] Copies fixed product options before start; NULL uses defaults. */
xiaotai_err_t xiaotai_video_configure(const xiaotai_video_config_t *config);

/* Engine lifecycle (called by the framework through the generated tables):
 * init is [init], start is [owner] (xiaotai_module_t in xiaotai/framework.h).
 * init registers the platform's backends and claims what must be taken before
 * any module initializes (decoders that hold scarce internal memory for the
 * life of the program). start, after every module has declared its
 * requirements, creates the tasks and reserves pools, encoder and scaler; it
 * must run before the first link is made. When an uplink was declared, start
 * first has the camera task call prepare() of every camera part that has one
 * (xiaotai/hal.h: claims the camera's resident frame memory and starts its
 * platform video subsystem once, without streaming), then reserves the
 * encoder, and returns when both are done: the camera's large contiguous
 * blocks and the encoder's internal memory are taken before the network
 * fragments memory, in the order the platform needs. Because start waits for
 * both, this is a reservation made by start (xiaotai/framework.h), not
 * bring-up left running on another task: both have finished when start
 * returns. A camera without prepare() is not touched before its first
 * session. A product that declared
 * nothing has no task and no buffer. Video never stops the product from
 * starting: a claim that fails in init removes that backend (the capabilities
 * no longer list its codec) and init still returns XIAOTAI_OK unless a
 * registry is full; a prepare or a reservation that fails in start leaves the
 * engine started without that direction, and sessions that need it get
 * XIAOTAI_ERR_NOT_SUPPORTED. Both are recorded in the event log. */
xiaotai_err_t xiaotai_engine_video_init(void);
void xiaotai_engine_video_start(void);
/* [owner] Host tests only: stop the engine's tasks, free its buffers and
 * tables and forget every requirement, backend, session and counter. */
void xiaotai_video_reset(void);

#ifdef __cplusplus
}
#endif
