// SPDX-License-Identifier: MIT
#pragma once
/* Hardware abstraction: parts described by descriptors.
 *
 * A board answers "which parts do I have": any number of each kind, an index
 * past the end returns NULL. Engines, the UI system and applications choose by
 * descriptor, never by part model, board or chip, so a new display, capture
 * path, input, output, camera, network or measured quantity is one more part.
 * A new kind of part appends one enumerator here and one entry in
 * sdk/xiaotai_hal/needs.json; no interface changes.
 *
 * Rules that hold for every part:
 * - A part object and everything it points to is immutable and lives until
 *   power-off: a pointer returned by a query never dangles and may be cached.
 * - Parts are hardware fixed on the board. A device that can be plugged in and
 *   out at run time (a USB microphone, a UVC camera) is not supported by this
 *   version; it will be added as a removable part under these rules, so that
 *   engines written now need no change of contract:
 *     - its object and index exist for the whole run (the slot); only its
 *       descriptor is meaningful while a device is present, and queries never
 *       start or stop returning it;
 *     - presence changes reach the part's [engine] through a callback of this
 *       interface (never from an interrupt), which an engine may republish as
 *       an event of its own;
 *     - the engine takes up an inserted device only while it has no session
 *       on that part (it then chooses processors, buffers and modes again);
 *       a removal during a session ends that session as a failure of the part,
 *       exactly like a device error;
 *     - a removable part satisfies no need (xiaotai/needs.h), so no build or
 *       start check relies on a device being plugged in.
 * - self is passed back unchanged as the first argument of every function.
 * - Functions return xiaotai_err_t (ESP-IDF codes). A NULL function pointer
 *   means the part lacks that capability; pointers marked "required" are never
 *   NULL. Callers own every buffer they pass; a part keeps no pointer to it
 *   after the call returns.
 * - No part function is called from an interrupt handler.
 * - On paths marked [rt] a part neither allocates nor logs: it counts, and
 *   stats() reads the counters.
 * - Memory a part allocates is requested by purpose (xiaotai/port/mem.h).
 * - No part function except creation ([board]) and functions tagged [flash]
 *   calls the flash driver or anything else that disables the cache (flash or
 *   partition reads and writes, NVS, OTA): the caller's stack, and the
 *   buffers it passes, may be in external RAM, which cannot be reached while
 *   the cache is off (ESP-IDF asserts). Persistent values go through the kv
 *   port (xiaotai/port/kv.h), whose worker runs the flash work on its own
 *   internal stack; reading data the build placed in flash (constants, a
 *   memory-mapped image) is plain memory access and allowed. The owning
 *   engine calls a [flash] function only from a task whose stack is in
 *   internal RAM. No function of this version is tagged [flash].
 * - open() that fails leaves the part closed and may be retried, except that
 *   XIAOTAI_ERR_INVALID_STATE from open() is final until restart (the part
 *   could not undo a half-finished start).
 *
 * Who may call a function: [glue], [board], [init], [any] and [rt] as defined
 * in xiaotai/framework.h ([rt] here: made by the [engine] from its real-time
 * task; every [any] query returns NULL until xiaotai_hal_init() has
 * succeeded), plus the abstraction's own:
 *   [engine]  the one component that owns the part (media: audio, UI:
 *             displays, interaction: inputs, video: cameras, network:
 *             networks; outputs: see "Output ownership" below). The owner
 *             serializes its calls: two calls on one part never overlap,
 *             whichever of its tasks makes them. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/codec.h"
#include "xiaotai/needs.h"
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------- audio */

/* Encoding of an audio part's stream: the audio values of the device's one
 * codec vocabulary (xiaotai/codec.h), so the media engine and links take a
 * part's format without converting: XIAOTAI_CODEC_PCM16 (interleaved signed
 * 16-bit frames, native byte order), XIAOTAI_CODEC_OPUS (one encoded packet
 * per read), XIAOTAI_CODEC_G711A, XIAOTAI_CODEC_G711U, and whatever audio
 * value is appended there. XIAOTAI_CODEC_NONE (0) is no format: a descriptor
 * always names its encoding, a zero-filled one is not PCM16 and satisfies
 * no PCM need. The former names stay as spellings of the same values. */
typedef xiaotai_codec_t xiaotai_audio_encoding_t;
#define XIAOTAI_AUDIO_PCM16 XIAOTAI_CODEC_PCM16
#define XIAOTAI_AUDIO_OPUS XIAOTAI_CODEC_OPUS
#define XIAOTAI_AUDIO_G711A XIAOTAI_CODEC_G711A

typedef enum {
    XIAOTAI_CHANNEL_UNUSED = 0,
    XIAOTAI_CHANNEL_MIC,        /* microphone a processor may use */
    XIAOTAI_CHANNEL_REFERENCE,  /* loopback of what the speaker plays (echo reference) */
    XIAOTAI_CHANNEL_MONO,       /* output: the single channel */
    XIAOTAI_CHANNEL_LEFT,
    XIAOTAI_CHANNEL_RIGHT,
    /* Microphone wired but not offered to processors (level diagnostics only):
     * the board measured that using it costs time and gains nothing. */
    XIAOTAI_CHANNEL_MONITOR,
} xiaotai_channel_role_t;

#define XIAOTAI_AUDIO_CHANNELS_MAX 8U

/* The stream format. Processors, encoders and resamplers are chosen from it. */
typedef struct {
    xiaotai_audio_encoding_t encoding;   /* an audio codec value, never XIAOTAI_CODEC_NONE */
    uint32_t rate_hz;
    uint8_t channels;
    xiaotai_channel_role_t roles[XIAOTAI_AUDIO_CHANNELS_MAX]; /* in wire (slot) order */
    uint8_t frame_ms;              /* encoded: packet duration; PCM16: 0 */
} xiaotai_audio_desc_t;

/* Monotonic counters since boot; a snapshot is not one transaction. */
typedef struct {
    uint32_t overruns;     /* input: blocks lost because nobody read in time; output: underruns */
    uint32_t lost_frames;  /* PCM frames (encoded: packets) lost in those */
    uint32_t errors;       /* device and wire errors: bad blocks, checksums, failed transfers */
} xiaotai_audio_stats_t;

#define XIAOTAI_AUDIO_COUPLING_UNKNOWN INT8_MIN

/* Capture facts beyond the stream format. */
typedef struct {
    int8_t gain_db;            /* gain on MIC channels after open() */
    int8_t gain_min_db;        /* range of set_gain_db(); min == max: fixed */
    int8_t gain_max_db;
    uint8_t gain_step_db;      /* requests round down to a multiple; 0 with a fixed gain */
    /* Worst-case power gain from the PCM written to the output of the same
     * group to the PCM read from MIC channels, with that output at 100 % volume
     * and this input at 0 dB gain; XIAOTAI_AUDIO_COUPLING_UNKNOWN when the
     * board did not measure it. Echo gates scale their thresholds from it. */
    int8_t echo_coupling_db;
    /* Non-zero: this part shares one device and clock with every audio part of
     * the same group. Opening any of them starts the whole device (capture
     * runs from then on), and the device stops only when all are closed.
     * REFERENCE channels carry the output of the same group. */
    uint8_t group;
    uint16_t settle_ms;        /* audio read this long after open() is a start transient */
    uint16_t read_timeout_ms;  /* read() returns within this bound; 0: unbounded */
    uint16_t block_frames;     /* device block (DMA descriptor, packet) in frames; 0: unknown */
} xiaotai_audio_input_info_t;

typedef struct xiaotai_audio_input {
    xiaotai_audio_desc_t desc;
    xiaotai_audio_input_info_t info;
    /* [engine] Required. Starts delivery. Called only when the engine is ready
     * to read every input of the group, because data flows from here on. */
    xiaotai_err_t (*open)(void *self);
    /* [rt] Required; one reader task. PCM16: capacity is a whole number of
     * frames and *size == capacity on success. Encoded: one packet, *size its
     * length (XIAOTAI_ERR_INVALID_SIZE and the packet is dropped when capacity
     * is too small). XIAOTAI_ERR_TIMEOUT: nothing arrived within
     * info.read_timeout_ms, read again. Any other error: progress is unknown,
     * discard the block. XIAOTAI_ERR_INVALID_STATE: not open. data may lie in
     * external RAM: a part whose driver cannot write there copies through a
     * buffer of its own. */
    xiaotai_err_t (*read)(void *self, void *data, size_t capacity, size_t *size);
    /* [engine] Stops delivery and drops what was queued; the part may power the
     * device down when its whole group is closed. open() restarts it. */
    xiaotai_err_t (*close)(void *self);
    /* [engine] Gain of the MIC channels, info.gain_min_db..gain_max_db;
     * XIAOTAI_ERR_INVALID_ARG outside. Takes effect within one device block. */
    xiaotai_err_t (*set_gain_db)(void *self, int8_t gain_db);
    /* [any] Never blocks. */
    void (*stats)(void *self, xiaotai_audio_stats_t *stats);
    void *self;
} xiaotai_audio_input_t;

enum {
    /* write() blocks until the device has taken the block: the sample clock
     * paces the writer. Without this flag write() returns at once and the
     * writer paces itself in real time. */
    XIAOTAI_AUDIO_OUTPUT_PACED = 1u << 0,
};

typedef struct {
    uint16_t latency_ms;        /* audio between write() returning and the speaker (device buffer); 0: unknown */
    uint16_t enable_ms;         /* set_enabled(true) blocks this long (amplifier start) */
    uint16_t max_write_frames;  /* largest block one write() takes; 0: any */
    /* Volume 1..100 % is linear in dB over this span, in 0.01 dB (4950: 1 % is
     * -49.5 dB, 100 % is 0 dB); 0 % is silent. 0: unknown. */
    uint16_t volume_span_cdb;
    uint8_t group;              /* see xiaotai_audio_input_info_t.group */
    uint8_t flags;              /* XIAOTAI_AUDIO_OUTPUT_* */
} xiaotai_audio_output_info_t;

typedef struct xiaotai_audio_output {
    xiaotai_audio_desc_t desc;
    xiaotai_audio_output_info_t info;
    /* [engine] Required. Starts the device with the amplifier off. Volume and
     * mute are set by the engine after every open(). */
    xiaotai_err_t (*open)(void *self);
    /* [rt] Required; whole interleaved frames, size in bytes, at most
     * info.max_write_frames. The part may modify data (in-place gain). On an
     * error progress is unknown: discard the block, never replay it. */
    xiaotai_err_t (*write)(void *self, void *data, size_t size);
    /* [engine] percent 0..100; XIAOTAI_ERR_INVALID_ARG above. */
    xiaotai_err_t (*set_volume)(void *self, uint8_t percent);
    xiaotai_err_t (*set_muted)(void *self, bool muted);
    /* [engine] Amplifier power. Enabling returns when the amplifier is ready
     * (info.enable_ms). Never stops capture or the clock of the group. */
    xiaotai_err_t (*set_enabled)(void *self, bool enabled);
    /* [any] Silence frames that, written after the last payload, push all of it
     * out of the device buffer (one device period of margin included); 0:
     * unknown. Does not prove acoustic output. */
    size_t (*flush_frames)(void *self);
    /* [engine] Amplifier off, then stop; see xiaotai_audio_input_t.close. */
    xiaotai_err_t (*close)(void *self);
    /* [any] Never blocks. */
    void (*stats)(void *self, xiaotai_audio_stats_t *stats);
    void *self;
} xiaotai_audio_output_t;

/* ----------------------------------------------------------------- display */

/* Manifests (board.json parts.display[].pixel, a theme's display.pixel) spell
 * a value as its name without XIAOTAI_PIXEL_, in lower case ("rgb565",
 * "mono1_vlsb"); the table is the vocabulary of sdk/xiaotai_hal/needs.json.
 * Values are only appended. */
typedef enum {
    XIAOTAI_PIXEL_RGB565 = 0,  /* 16 bits per pixel, row-major */
    XIAOTAI_PIXEL_MONO1,       /* 1 bit per pixel, row-major, most significant bit left */
    /* 24 bits per pixel, row-major, 3 bytes in memory: blue, green, red (the
     * value 0xRRGGBB stored little-endian, as RGB565 is). */
    XIAOTAI_PIXEL_RGB888,
    /* 1 bit per pixel in vertical bytes: each byte is 8 rows of one column,
     * least significant bit on top; bytes run left to right, then page by page
     * downwards (the native layout of SSD1306-class panels). */
    XIAOTAI_PIXEL_MONO1_VLSB,
} xiaotai_pixel_t;

/* How the renderer hands frames to the panel. */
typedef enum {
    /* Draw into a buffer of transfer_pixels pixels and present dirty areas. */
    XIAOTAI_RENDER_PARTIAL = 0,
    /* Draw into one whole-frame buffer the renderer owns (XIAOTAI_MEM_BULK);
     * the panel takes transfer_pixels per transfer (0: the whole frame). For
     * panels that do not commit small areas reliably, and for frame-oriented
     * panels. */
    XIAOTAI_RENDER_FULL,
    /* The panel owns `buffers` frame buffers and scans them out: draw in place
     * and present a whole buffer to show it. */
    XIAOTAI_RENDER_SCANOUT,
} xiaotai_render_t;

enum {
    XIAOTAI_DISPLAY_SWAP_BYTES = 1u << 0,   /* RGB565 goes out high byte first: the renderer swaps */
    XIAOTAI_DISPLAY_DIMMABLE = 1u << 1,     /* set_backlight honours 1..99; otherwise non-zero is on */
    XIAOTAI_DISPLAY_DMA_BUFFER = 1u << 2,   /* the buffer given to present() is read by DMA: see buffer_align */
    XIAOTAI_DISPLAY_EVEN_AREAS = 1u << 3,   /* presented areas start on an even column and end on an odd one */
    /* Refreshing is slow or wears the panel, and partial updates are limited
     * (e-paper): present() may block for a long time and should be called
     * as seldom as possible. Themes match on it; nothing else changes. */
    XIAOTAI_DISPLAY_SLOW_REFRESH = 1u << 4,
};

typedef struct {
    uint16_t width, height;        /* logical, after rotation */
    xiaotai_pixel_t pixel;
    bool touch;                    /* read_touch is present */
    bool round;
    xiaotai_render_t render;
    /* Quarter turns clockwise the renderer applies to put the logical frame on
     * the panel's native frame (0..3). Panels turned by their own registers
     * report 0. The native frame is width x height for 0 and 2, height x width
     * for 1 and 3. */
    uint8_t rotation;
    uint8_t buffers;               /* SCANOUT: 1 or 2; otherwise 0 */
    /* XIAOTAI_DISPLAY_DMA_BUFFER: the renderer's buffer comes from
     * XIAOTAI_MEM_DMA (PARTIAL) or XIAOTAI_MEM_BULK (FULL) with this alignment
     * in bytes (0: none). */
    uint8_t buffer_align;
    uint32_t transfer_pixels;      /* PARTIAL: draw buffer size; FULL: pixels per transfer, 0 whole frame */
    uint32_t flags;                /* XIAOTAI_DISPLAY_* */
} xiaotai_display_desc_t;

/* What a platform renderer may use instead of present(): the port's own
 * handles. On ESP-IDF: esp_lcd_panel_handle_t, esp_lcd_panel_io_handle_t and
 * esp_lcd_touch_handle_t; on the host all NULL. The part owns them; the
 * renderer borrows them for the life of the display. */
enum {
    XIAOTAI_SCANOUT_UNSPECIFIED = 0,
    XIAOTAI_SCANOUT_DPI,        /* ESP-IDF MIPI DSI's DPI framebuffer engine */
};

typedef struct xiaotai_display_handles {
    void *panel;
    void *panel_io;                /* NULL on buses without one */
    void *touch;                   /* NULL without touch */
    void *framebuffers[2];         /* SCANOUT: the panel's frame buffers, desc.buffers of them */
    /* Orientation the part already applied in the panel's registers; a
     * renderer that programs panel orientation must repeat exactly this. */
    bool swap_xy, mirror_x, mirror_y;
    uint8_t scanout_kind;       /* XIAOTAI_SCANOUT_* before using native events */
} xiaotai_display_handles_t;

typedef struct {
    uint16_t x, y, width, height;  /* in the panel's native frame */
} xiaotai_rect_t;

/* A rectangle in the display's logical frame: desc.width x desc.height, after
 * rotation, the frame the UI draws in and in which display regions are placed
 * (a video area, a theme's slot). It is the native xiaotai_rect_t only when
 * desc.rotation is 0; whoever calls present() converts with the rotation
 * first, so the two are never interchangeable. The display rectangles of the
 * UI system (xiaotai_ui_rect_t) and the video engine (xiaotai_video_area_t)
 * have the same members in the same order and are defined as aliases of this
 * one, so code using their names does not change. */
typedef struct {
    uint16_t x, y, width, height;
} xiaotai_display_area_t;

typedef struct {
    uint16_t x, y;                 /* in the panel's native frame, like present() */
} xiaotai_touch_point_t;

/* During open_bound(), the binder may use the handles and present() for
 * initial scanout under the existing serialized [engine] contract. A new
 * display is not yet open for touch/backlight. Do not publish the handles
 * or start another display task during the bind. On error set released=true
 * only after all accesses and native callbacks using the handles have stopped. */
typedef xiaotai_err_t (*xiaotai_display_bind_fn)(void *ctx, const xiaotai_display_handles_t *handles,
                                                bool *released);

typedef struct xiaotai_display {
    xiaotai_display_desc_t desc;
    /* [engine] Required. Powers and initializes panel and touch; the backlight
     * stays off. handles may be NULL. Opening again returns the same handles.
     * A failed start is rolled back completely. */
    xiaotai_err_t (*open)(void *self, xiaotai_display_handles_t *handles);
    /* [engine] 0: off. XIAOTAI_ERR_INVALID_STATE before open(),
     * XIAOTAI_ERR_INVALID_ARG above 100. */
    xiaotai_err_t (*set_backlight)(void *self, uint8_t percent);
    /* [engine] Required. Shows pixels (desc.pixel, native orientation, rows
     * tightly packed) in area and returns when the part no longer reads them.
     * The area may be of any size: the part splits it into the transfers its
     * bus takes (desc.transfer_pixels is for renderers that drive the handles
     * themselves). MONO1_VLSB and SCANOUT take whole frames only; a SCANOUT
     * buffer is shown by switching to it, any other source is copied. */
    xiaotai_err_t (*present)(void *self, const xiaotai_rect_t *area, const void *pixels);
    /* [engine] Polled by the renderer. *count: touched points written, at most
     * capacity; 0 when released. At most one bus transfer per poll except
     * GT911, which needs up to three (status, optional data, acknowledge).
     * GT911 uses finite waits for each transfer, not one wall-clock
     * deadline for the whole poll. */
    xiaotai_err_t (*read_touch)(void *self, xiaotai_touch_point_t *points, size_t capacity, size_t *count);
    void *self;
    /* [engine] Optional synchronous startup. Existing hardware is borrowed:
     * a failed bind never closes it. On a new open a failed bind rolls back
     * only if it released the handles, otherwise the part retains its
     * resources and refuses another open. A second open during startup is
     * rejected with BUSY. Ordinary open() stays idempotent when not starting. */
    xiaotai_err_t (*open_bound)(void *self, xiaotai_display_bind_fn bind, void *ctx);
} xiaotai_display_t;

/* ------------------------------------------------------------------- input */

/* Pointer input belongs to its display (read_touch); touch keys are buttons. */
typedef enum {
    XIAOTAI_INPUT_BUTTON = 0,
    XIAOTAI_INPUT_ENCODER,
    XIAOTAI_INPUT_CARD,        /* reader: XIAOTAI_INPUT_DATA carries the card number */
    XIAOTAI_INPUT_VOICE,       /* voice chip: XIAOTAI_INPUT_DATA carries the spotted command */
    /* A switch that holds a state (door contact, reed or tamper switch), not
     * a key: PRESSED when it becomes active, RELEASED when it returns, and,
     * unlike a key, its state at open() is reported by the first poll. */
    XIAOTAI_INPUT_CONTACT,
    /* A control with a position rather than steps (slider, potentiometer,
     * a sensor threshold dial): XIAOTAI_INPUT_LEVEL with read_level(). */
    XIAOTAI_INPUT_SLIDER,
} xiaotai_input_kind_t;

/* Events reported by poll(). A part reports either edges (PRESSED, RELEASED)
 * and leaves gestures to the interaction engine and its profile timing, or,
 * when the hardware only delivers finished gestures, the gesture bits. */
enum {
    XIAOTAI_INPUT_CLICK = 1u << 0,
    XIAOTAI_INPUT_LONG_PRESS = 1u << 1,
    XIAOTAI_INPUT_DOUBLE_CLICK = 1u << 2,
    XIAOTAI_INPUT_TURN_UP = 1u << 3,     /* one step per poll */
    XIAOTAI_INPUT_TURN_DOWN = 1u << 4,
    /* Debounced edges. A key already held when the part opens reports nothing
     * until it has been released once, so a reset with the key held never
     * starts a gesture. */
    XIAOTAI_INPUT_PRESSED = 1u << 5,
    XIAOTAI_INPUT_RELEASED = 1u << 6,
    XIAOTAI_INPUT_DATA = 1u << 7,        /* fetch the payload with read() before the next poll */
    /* The level moved since the last LEVEL (by at least the part's own
     * hysteresis); read_level() gives the new one. The first poll after
     * open() reports the level the control is at. */
    XIAOTAI_INPUT_LEVEL = 1u << 8,
};

typedef struct {
    xiaotai_input_kind_t kind;
    const char *role;              /* "main", "volume_up", "volume_down", "doorbell", "voice", "door_contact", ... */
    uint32_t events;               /* XIAOTAI_INPUT_* bits this part can report */
    int32_t level_min, level_max;  /* XIAOTAI_INPUT_LEVEL: range of read_level(); otherwise 0 */
} xiaotai_input_desc_t;

#define XIAOTAI_INPUT_POLL_MS 10U
#define XIAOTAI_INPUT_TEXT_MAX 64U

typedef struct xiaotai_input {
    xiaotai_input_desc_t desc;
    /* [engine] Required. */
    xiaotai_err_t (*open)(void *self);
    /* [engine] Required. Called every XIAOTAI_INPUT_POLL_MS by one task;
     * returns the event bits since the previous poll. Bounded: at most one
     * short bus transfer, and parts on a bus sample it less often by themselves. */
    uint32_t (*poll)(void *self);
    /* [engine] Payload of the last XIAOTAI_INPUT_DATA as NUL-terminated text,
     * at most XIAOTAI_INPUT_TEXT_MAX bytes with the terminator.
     * XIAOTAI_ERR_NOT_FOUND when none is pending. */
    xiaotai_err_t (*read)(void *self, char *text, size_t capacity);
    /* [engine] Required with XIAOTAI_INPUT_LEVEL: the level of the last LEVEL
     * event, desc.level_min..level_max. Never blocks (the value was sampled by
     * poll()). XIAOTAI_ERR_NOT_FOUND before the first LEVEL. */
    xiaotai_err_t (*read_level)(void *self, int32_t *level);
    void *self;
} xiaotai_input_t;

/* ------------------------------------------------------------------ output */

typedef enum {
    XIAOTAI_OUTPUT_LIGHT = 0,      /* single colour, level in red */
    XIAOTAI_OUTPUT_RGB_LIGHT,
    XIAOTAI_OUTPUT_VIBRATOR,       /* strength in red */
    XIAOTAI_OUTPUT_RELAY,          /* on when red != 0 */
} xiaotai_output_kind_t;

typedef struct {
    xiaotai_output_kind_t kind;
    /* "status", "door", "door.gate", ...: a family name, optionally followed
     * by "." and a name of its own (see "Output ownership"). */
    const char *role;
    uint16_t pixels;               /* individually set elements (LED strip); 1 for a single element */
} xiaotai_output_desc_t;

typedef struct xiaotai_output {
    xiaotai_output_desc_t desc;
    /* [engine] Required. Sets every element. Blocks at most 20 ms. */
    xiaotai_err_t (*set)(void *self, uint8_t red, uint8_t green, uint8_t blue);
    /* [engine] One frame for a part with several pixels: red, green, blue per
     * pixel, in the board's left-to-right order, count == desc.pixels. Limits
     * the board needs (total brightness) are applied by the part. */
    xiaotai_err_t (*write)(void *self, const uint8_t *rgb, size_t count);
    void *self;
} xiaotai_output_t;

/* Output ownership. The role family of an output is its role up to the first
 * "." ("door.gate" is in family "door"). A component that drives the outputs
 * of a family itself (the lock module: "door") claims the family in its
 * init(); from then on only the claimant calls those parts. Every output of an
 * unclaimed family belongs to the feedback engine, which drives the kinds it
 * renders and leaves the others untouched. Ownership is thus decided by the
 * components a product selects, never by this interface: a new kind of owned
 * output (a gate barrier, a siren, a smart-plug switch) is a new component
 * that claims its family. */
typedef struct xiaotai_output_claim {
    const char *family;            /* [a-z0-9_]+, no "." */
    const char *owner;             /* manifest name of the claiming component, for diagnostics */
    struct xiaotai_output_claim *next;   /* used by this interface; leave it zero */
} xiaotai_output_claim_t;

/* [init] Records claim, which has static storage and is not changed
 * afterwards. Claims are made only while the framework runs the init()
 * functions (on the owner task, before any start()), so start() and every
 * later reader see the complete set. Claiming a family the same owner already
 * claimed is XIAOTAI_OK and changes nothing (a framework restarted in a host
 * test). XIAOTAI_ERR_INVALID_ARG: NULL, an empty owner, or a family that is
 * empty or holds anything but [a-z0-9_]. XIAOTAI_ERR_INVALID_STATE: another
 * owner claimed the family. Works before xiaotai_hal_init(). */
xiaotai_err_t xiaotai_hal_output_claim(xiaotai_output_claim_t *claim);

/* [any] Owner of the part's role family; NULL when the family is unclaimed
 * (the feedback engine's part) or part is NULL. Complete once every init()
 * has run. */
const char *xiaotai_hal_output_owner(const xiaotai_output_t *part);

/* ------------------------------------------------------------------ camera */

enum {
    XIAOTAI_CAMERA_YUV422 = 1u << 0,   /* packed YUYV, 2 bytes per pixel */
    XIAOTAI_CAMERA_JPEG = 1u << 1,
    XIAOTAI_CAMERA_RGB565 = 1u << 2,
    /* 12 bits per pixel packed by row pairs: the first row of each pair is
     * U Y Y, the second V Y Y (not planar I420). */
    XIAOTAI_CAMERA_YUV420_PAIRED = 1u << 3,
};

enum {
    XIAOTAI_CAMERA_FULL_RANGE = 1u << 0,  /* YUV uses the full 0..255 range (BT.601) */
};

typedef struct {
    uint32_t formats;              /* XIAOTAI_CAMERA_* format bits open() accepts */
    uint16_t max_width, max_height;
    /* Equal to the maximum: the camera has one size, callers open it and
     * transform. 0: only what open() accepts is known. */
    uint16_t min_width, min_height;
    uint8_t max_fps;               /* 0: unknown */
    /* Quarter turns clockwise that make the image upright on a device held as
     * its display is read (mounting of the sensor). */
    uint8_t rotation;
    /* Frames carry rows up to the next multiple of this (encoders that need
     * whole macroblocks); 0 or 1: no padding. */
    uint8_t row_align;
    uint8_t flags;                 /* XIAOTAI_CAMERA_FULL_RANGE */
} xiaotai_camera_desc_t;

typedef struct {
    uint16_t width, height;
    uint32_t format;               /* exactly one format bit of desc.formats */
    /* A hint; 0: the sensor's default. A part that cannot run this rate opens
     * at the nearest one it can and never refuses a mode for its rate alone
     * (the caller takes frames at its own pace). */
    uint8_t fps;
} xiaotai_camera_mode_t;

typedef struct {
    const uint8_t *data;
    size_t size;                   /* bytes valid in data; may include a device tail */
    uint16_t width, height;        /* visible pixels */
    uint16_t rows;                 /* rows present in data: height, or height padded to desc.row_align */
    uint32_t format;               /* one XIAOTAI_CAMERA_* format bit */
    int64_t captured_us;           /* xiaotai_time_us() clock; 0: unavailable */
    uint32_t sequence;             /* 0: unavailable */
    uint32_t skipped;              /* older frames the part dropped to deliver this one */
} xiaotai_camera_frame_t;

/* One worker task owns all calls, in this order: prepare() once, when the part
 * has it; then for each session set_power(true) (returns after the sensor
 * settled), re-check that the session is still wanted, open(),
 * borrow/release..., close(), set_power(false). After an open() attempt,
 * close() before powering down or opening another mode. */
typedef struct xiaotai_camera {
    xiaotai_camera_desc_t desc;
    /* [engine] Optional; NULL: the part takes nothing before its first
     * set_power(true). Takes what the camera keeps for the whole run (its
     * frame memory) and starts the platform's video subsystem for it once,
     * without streaming. Called at most once, before the first set_power(), so
     * that the engine can take the camera's large contiguous blocks while it
     * starts, before any network link fragments memory. It may power the
     * sensor to start the subsystem; afterwards the sensor stays powered only
     * if the part always keeps it powered once started, otherwise it is off.
     * XIAOTAI_ERR_NO_MEM: the frame memory could not be taken. Any failure
     * leaves the part as if prepare() had not been called: the first
     * set_power(true) takes what is missing, or fails. */
    xiaotai_err_t (*prepare)(void *self);
    /* [engine] Required. */
    xiaotai_err_t (*set_power)(void *self, bool enabled);
    /* [engine] Required. XIAOTAI_ERR_NOT_SUPPORTED: the camera cannot do this
     * mode (also when the sensor found is not the one the board declares; the
     * part then still needs close()). XIAOTAI_ERR_INVALID_STATE: already open. */
    xiaotai_err_t (*open)(void *self, const xiaotai_camera_mode_t *mode);
    /* [engine] Required. XIAOTAI_ERR_INVALID_STATE while a frame is on loan. */
    xiaotai_err_t (*close)(void *self);
    /* [engine] Required. Never blocks: XIAOTAI_ERR_NOT_FOUND when no frame is
     * ready. Delivers the newest frame. One loan at a time, read-only, until
     * release() of this same descriptor; release before close and before
     * power-down. */
    xiaotai_err_t (*borrow)(void *self, xiaotai_camera_frame_t *frame);
    xiaotai_err_t (*release)(void *self, xiaotai_camera_frame_t *frame);
    void *self;
} xiaotai_camera_t;

/* ----------------------------------------------------------------- network */

typedef enum {
    XIAOTAI_NETWORK_WIFI = 0,          /* the chip's own Wi-Fi */
    XIAOTAI_NETWORK_WIFI_COPROCESSOR,  /* Wi-Fi on a coprocessor */
    XIAOTAI_NETWORK_ETHERNET,
    XIAOTAI_NETWORK_CELLULAR,
} xiaotai_network_kind_t;

enum {
    XIAOTAI_PROVISION_PORTAL = 1u << 0,   /* access point + setup page */
    XIAOTAI_PROVISION_SERIAL = 1u << 1,   /* AT over the console */
    XIAOTAI_PROVISION_BLE = 1u << 2,
};

typedef struct {
    xiaotai_network_kind_t kind;
    uint32_t provisioning;         /* XIAOTAI_PROVISION_* the board supports */
} xiaotai_network_desc_t;

/* Board facts for the drivers of the two Wi-Fi kinds (xiaotai/net_driver.h),
 * pointed to by xiaotai_network_t.driver_config. Each further kind defines its
 * facts (an Ethernet PHY address and pins, a modem's UART and power key) in
 * its part library header, where both the board and the driver see them.
 * Zero is the default of every field; fields are only appended. */
typedef struct {
    /* XIAOTAI_NETWORK_WIFI_COPROCESSOR: the coprocessor allocates transmit
     * buffers on demand and keeps no cached ones, leaving its internal RAM to
     * receive bursts. The chip's own Wi-Fi ignores it. */
    bool tx_buffers_on_demand;
} xiaotai_network_wifi_t;

/* Bringing up link hardware that has firmware of its own (a coprocessor: the
 * chip that runs the network stack for this one). "Coprocessor" is the only
 * name for it in the SDK, so that no hardware name collides with an
 * application's. Stages up to READY are progress; the ones after it are final
 * outcomes. */
typedef enum {
    XIAOTAI_NETWORK_CHECKING = 0,  /* waiting for the coprocessor, reading its identity */
    XIAOTAI_NETWORK_VERIFYING,     /* checking the firmware image to write */
    XIAOTAI_NETWORK_WRITING,       /* percent runs 0..100 */
    XIAOTAI_NETWORK_ACTIVATING,
    XIAOTAI_NETWORK_RESTARTING,    /* written and activated: the caller restarts the system */
    XIAOTAI_NETWORK_READY,
    XIAOTAI_NETWORK_NO_LINK,       /* the coprocessor did not answer; retry manually */
    XIAOTAI_NETWORK_UNFINISHED,    /* an earlier update did not complete; only a manual retry proceeds */
    XIAOTAI_NETWORK_BAD_IMAGE,     /* the image to write failed its checks; nothing was written */
    XIAOTAI_NETWORK_UNSUPPORTED,   /* coprocessor firmware this board cannot work with */
    XIAOTAI_NETWORK_POWER_CYCLE,   /* a transfer failed midway; power off and on before retrying */
} xiaotai_network_stage_t;

/* Called on the task that runs prepare() or coprocessor_update(). */
typedef void (*xiaotai_network_progress_fn_t)(xiaotai_network_stage_t stage, uint8_t percent, void *ctx);

#define XIAOTAI_COPROCESSOR_TEXT_MAX 32U

typedef struct {
    char project[XIAOTAI_COPROCESSOR_TEXT_MAX];
    char version[XIAOTAI_COPROCESSOR_TEXT_MAX];
    char platform[XIAOTAI_COPROCESSOR_TEXT_MAX];   /* SDK version the firmware was built with */
    char protocol[XIAOTAI_COPROCESSOR_TEXT_MAX];   /* host link protocol version */
    char elf_sha256[65];                           /* hexadecimal; may be a prefix */
} xiaotai_coprocessor_info_t;

/* Called synchronously during prepare. The receiver copies the snapshot
 * before returning; NULL withdraws it without issuing a new link request. */
typedef void (*xiaotai_network_identity_fn_t)(const xiaotai_coprocessor_info_t *info, void *ctx);

/* The network implementation for the kind is a component the build selects;
 * the part carries the board's facts and the hooks only the board can do. The
 * network engine is the only caller. It never runs two hooks at once, and it
 * runs none (coprocessor_info excepted) while a real-time link (xiaotai/link.h)
 * or a media session is up: its caller quiesces the system first
 * (xiaotai_quiesce_begin in xiaotai/framework.h). "Link" here never means the
 * Wi-Fi link: coprocessor_update and coprocessor_command need the
 * coprocessor's own connection to the host, and the network may be up while
 * they run. */
typedef struct xiaotai_network {
    xiaotai_network_desc_t desc;
    /* Board facts for the kind's driver, read in its start():
     * xiaotai_network_wifi_t for the Wi-Fi kinds, the kind's own struct
     * otherwise. Static storage. NULL: the driver's defaults (all zero). */
    const void *driver_config;
    /* [engine] Makes the link hardware usable before the network starts:
     * power, reset, handshake with the coprocessor, firmware check and, when the
     * board carries a newer image, update. Blocks (seconds; minutes when
     * writing). progress may be NULL. manual_retry: the user asked again after
     * a final stage. XIAOTAI_OK with READY: start the network. XIAOTAI_OK with
     * RESTARTING: restart the system. Otherwise the last stage reported tells
     * why; an error returned without a final stage having been reported is
     * published by the network engine as XIAOTAI_NET_FAULT_ERROR
     * (xiaotai/net.h) with that error. NULL: nothing to prepare. */
    xiaotai_err_t (*prepare)(void *self, bool manual_retry, xiaotai_network_progress_fn_t progress, void *ctx);
    /* [engine] Identity of the coprocessor firmware. live: ask the coprocessor
     * now (blocks); otherwise what prepare() read. XIAOTAI_ERR_NOT_FOUND when
     * it is not known or not complete. */
    xiaotai_err_t (*coprocessor_info)(void *self, bool live, xiaotai_coprocessor_info_t *info);
    /* [engine] Writes a firmware image the caller obtained and verified against
     * its digest; the part checks that the image is for this coprocessor,
     * writes and activates it, waits as long as the coprocessor needs, and
     * returns XIAOTAI_OK when the caller should restart the system. On an
     * error the running firmware is untouched unless the last stage was
     * POWER_CYCLE. */
    xiaotai_err_t (*coprocessor_update)(void *self, const uint8_t *image, size_t size,
                                        xiaotai_network_progress_fn_t progress, void *ctx);
    /* [engine] Diagnostic command to the coprocessor firmware, reply as text
     * (NUL-terminated, truncated to capacity). The vocabulary is the
     * coprocessor's. A command after which the coprocessor restarts with other
     * firmware (a rollback; which commands do is the part's configuration)
     * returns XIAOTAI_OK only when the coprocessor is ready for the host to
     * restart: how long that takes is a fact of the coprocessor firmware, so
     * the part waits and the engine keeps no delay of its own. */
    xiaotai_err_t (*coprocessor_command)(void *self, const char *command, char *reply, size_t capacity);
    void *self;
    /* [engine] Optional form of prepare(), for parts with a running-image
     * snapshot during the check. The engine starts with an empty cache and
     * the part reports changes on this same worker, outside its own lock.
     * A provider also implements prepare() and coprocessor_info(). */
    xiaotai_err_t (*prepare_with_identity)(void *self, bool manual_retry, xiaotai_network_progress_fn_t progress,
                                          xiaotai_network_identity_fn_t identity, void *ctx)
#ifdef __cplusplus
        = nullptr
#endif
        ;
} xiaotai_network_t;

/* ------------------------------------------------------------- measurement */

/* What a measurement part reports. Every kind has one fixed unit, given in
 * thousandths so that an int32_t carries the resolution sensors deliver. A new
 * quantity is one more value here. */
typedef enum {
    XIAOTAI_MEASURE_TEMPERATURE = 0,  /* millidegrees Celsius */
    XIAOTAI_MEASURE_HUMIDITY,         /* relative humidity, 0.001 % */
    XIAOTAI_MEASURE_BATTERY_CHARGE,   /* state of charge, 0.001 % (0..100000) */
    XIAOTAI_MEASURE_VOLTAGE,          /* millivolts */
    XIAOTAI_MEASURE_ILLUMINANCE,      /* millilux */
} xiaotai_measure_kind_t;

typedef struct {
    xiaotai_measure_kind_t kind;
    const char *role;              /* "room", "battery", "board", ... */
    /* The part takes a new sample at most this often and answers read() from
     * the last one in between; 0: every read() samples. */
    uint32_t sample_ms;
} xiaotai_measurement_desc_t;

/* A quantity the board can measure (temperature, battery charge, ambient
 * light). Values change slowly: readers poll at their own pace and there are
 * no events. A part that is a control rather than a quantity (a slider) is an
 * input (XIAOTAI_INPUT_SLIDER). */
typedef struct xiaotai_measurement {
    xiaotai_measurement_desc_t desc;
    /* [any] Required; not from a real-time task. Unlike other parts a
     * measurement has no owner: any component may read it, and the part
     * serializes its own bus access. The first call starts the sensor. Blocks
     * for at most one bus transfer and the sensor's conversion time.
     * XIAOTAI_ERR_NOT_FOUND: no valid value yet (the sensor is still starting);
     * any other error: the sensor did not answer, read again later. */
    xiaotai_err_t (*read)(void *self, int32_t *value);
    void *self;
} xiaotai_measurement_t;

/* ------------------------------------------------------------- board query */

/* The strings come from the board package's manifest (board.json "identity")
 * and are kept byte for byte: deployed devices already report them. */
typedef struct {
    const char *model;           /* "<vendor>-<board>", lower case */
    const char *revision;
    /* Hardware the device profile reports to the platform ("hardware":
     * board_model, chip_model). NULL: the board reports no device profile
     * hardware. */
    const char *profile_model;
    const char *profile_chip;
} xiaotai_board_identity_t;

/* [glue] Once, by xiaotai_start() after the glue, before any part is queried: the board brings up what its parts
 * share (buses, power rails, expanders) and creates its part objects. No part
 * is opened. Returns the board's first failure; a second call returns the same
 * result without repeating the work. Until it has returned XIAOTAI_OK every
 * part query returns NULL. It does not judge the parts against the board's
 * manifest: xiaotai_start() compares xiaotai_hal_needs() with the needs the
 * manifest declares. */
xiaotai_err_t xiaotai_hal_init(void);

/* [any] The strings are static. */
void xiaotai_hal_identity(xiaotai_board_identity_t *identity);
/* [any] Part by index; NULL past the end. Indexes are dense and stable. */
const xiaotai_audio_input_t *xiaotai_hal_audio_input(size_t index);
const xiaotai_audio_output_t *xiaotai_hal_audio_output(size_t index);
const xiaotai_display_t *xiaotai_hal_display(size_t index);
const xiaotai_input_t *xiaotai_hal_input(size_t index);
const xiaotai_output_t *xiaotai_hal_output(size_t index);
const xiaotai_camera_t *xiaotai_hal_camera(size_t index);
const xiaotai_network_t *xiaotai_hal_network(size_t index);
const xiaotai_measurement_t *xiaotai_hal_measurement(size_t index);

/* [any] First part with the given role (inputs, outputs, measurements); NULL
 * when absent. */
const xiaotai_input_t *xiaotai_hal_input_by_role(const char *role);
const xiaotai_output_t *xiaotai_hal_output_by_role(const char *role);
const xiaotai_measurement_t *xiaotai_hal_measurement_by_role(const char *role);

/* [any] XIAOTAI_NEED_* bits (xiaotai/needs.h) this board satisfies: derived
 * from its parts, plus external RAM as the memory port reports it. The rules
 * (which part kind or descriptor field gives which need) have one source,
 * sdk/xiaotai_hal/needs.json, which the build applies to board.json the same
 * way. */
uint32_t xiaotai_hal_needs(void);

/* Text sink of xiaotai_hal_report(): one line per call, without a line end.
 * The text is valid only during the call. */
typedef void (*xiaotai_hal_write_fn_t)(void *ctx, const char *line);

/* [any] Hardware diagnostics of the board as "name=value ..." lines: registers
 * of power and expander chips, bus and pin states, part counters. Blocks for
 * bus transfers (tens of milliseconds), so never from a real-time task. Opens
 * and changes nothing. A board without diagnostics writes no line. */
void xiaotai_hal_report(xiaotai_hal_write_fn_t write, void *ctx);

#ifdef __cplusplus
}
#endif
