// SPDX-License-Identifier: MIT
#pragma once
/* Video engine backends: encoders, decoders and picture transforms.
 *
 * The engine's pipelines are portable; the work that needs a codec library or
 * an accelerator is done by backends registered here. Each kind is a registry:
 * the engine asks every registered backend for a match score and uses the
 * best, so a new codec, accelerator or chip is one more backend, never a change
 * to the engine. A platform's backends live in this component's esp-idf/ (or
 * host/) directory and are registered by xiaotai_video_port_init(); host tests
 * register fakes.
 *
 * Descriptors are static storage and are kept by pointer. Backend functions are
 * called on the engine's tasks, one call at a time per opened state; they may
 * block on their hardware but never call back into the engine. All buffers they
 * are given are engine memory: XIAOTAI_MEM_BULK, aligned to the descriptor's
 * output_alignment, with a length that is a multiple of it. Memory a backend
 * needs for itself is requested by purpose (xiaotai/port/mem.h) in open(),
 * never in match() and never per frame. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"
#include "xiaotai/video_config.h"
#include "xiaotai/video_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* How a backend's calls load the CPU; decides how its task is scheduled, so no
 * priority number is tied to a codec or a chip. */
typedef enum {
    /* Short calls that mostly wait for a hardware codec or accelerator: the
     * task runs at XIAOTAI_VIDEO_HARDWARE_TASK_PRIORITY (xiaotai/video_config.h),
     * which a product keeps below every audio task. */
    XIAOTAI_VIDEO_LOAD_HARDWARE = 0,
    /* Long CPU bursts: the task runs below the UI, audio and network control. */
    XIAOTAI_VIDEO_LOAD_SOFTWARE,
} xiaotai_video_load_t;

/* Most backends of one kind: XIAOTAI_VIDEO_BACKENDS_MAX, a build option
 * (xiaotai/video_config.h). */

/* ----------------------------------------------------------------- encoders */

typedef struct {
    xiaotai_video_codec_t codec;
    xiaotai_video_pixel_t pixel;      /* layout of the pictures given to encode() */
    uint16_t width, height;           /* the picture given and the picture encoded */
    uint16_t rows;                    /* rows each given picture carries, >= height */
    bool full_range;                  /* signalled in the stream where the codec can */
    uint8_t fps;
    uint32_t bitrate_bps;             /* H.264 */
    uint8_t min_qp, max_qp;           /* H.264 */
    uint16_t key_interval_frames;     /* H.264: 1..255 */
    uint8_t quality;                  /* JPEG: 1..100 */
    /* Where encoded frames are written, for as long as the encoder is open. */
    uint8_t *output;
    size_t output_capacity;
} xiaotai_video_encoder_config_t;

typedef struct xiaotai_video_encoder {
    const char *name;
    xiaotai_video_load_t load;
    size_t output_alignment;          /* of config->output; 0 or 1: none */
    /* Contiguous XIAOTAI_MEM_DMA bytes open() takes for this config (reference
     * frames of a hardware encoder); 0: none. The engine keeps the encoder of
     * the declared profile with the largest value open as its reserve. Must
     * not allocate. May be NULL: 0. */
    size_t (*internal_bytes)(const xiaotai_video_encoder_config_t *config);
    /* 0: cannot encode this; higher is better. Must not allocate. */
    int (*match)(const xiaotai_video_encoder_config_t *config);
    /* An open encoder may be kept idle for any time and across sessions. On
     * failure *state is NULL and nothing is held. */
    xiaotai_err_t (*open)(const xiaotai_video_encoder_config_t *config, void **state);
    /* Apply a config that differs only in fps, bitrate_bps, key_interval_frames
     * or quality without closing. XIAOTAI_ERR_NOT_SUPPORTED: the caller closes
     * and opens instead. May be NULL: always close and open. */
    xiaotai_err_t (*reconfigure)(void *state, const xiaotai_video_encoder_config_t *config);
    /* Encode one picture; image->data is valid during the call only and is not
     * modified. On success packet->data points into config->output and stays
     * valid until the next encode or close; packet->key_frame tells what was
     * produced. force_key makes this frame a key frame. A result that does not
     * fit the output is XIAOTAI_ERR_INVALID_SIZE. */
    xiaotai_err_t (*encode)(void *state, const xiaotai_video_image_t *image, bool force_key,
                            xiaotai_video_packet_t *packet);
    void (*close)(void *state);
} xiaotai_video_encoder_t;

/* ----------------------------------------------------------------- decoders */

enum {
    /* H.264: decodes constrained baseline only. The engine checks each
     * sequence header and reports other streams as unsupported instead of
     * feeding them to the decoder. */
    XIAOTAI_VIDEO_DECODER_BASELINE_ONLY = 1u << 0,
    /* Opened once, in xiaotai_engine_video_init(), with the largest picture it
     * supports, and kept: it claims scarce internal memory that must be taken
     * before the network fragments it. */
    XIAOTAI_VIDEO_DECODER_PERSISTENT = 1u << 1,
    /* The decoder may overwrite bytes in the engine's exclusive, mutable
     * compressed-frame slot. The engine inspects the bitstream first and
     * never reads its bytes again after decode. Without this flag, input is
     * read-only. Neither kind may retain the slot after the call. */
    XIAOTAI_VIDEO_DECODER_MUTATES_INPUT = 1u << 2,
};

typedef struct {
    xiaotai_video_codec_t codec;
    uint16_t max_width, max_height;   /* largest picture written to the output; 0 x 0: the backend's own limit (PERSISTENT) */
} xiaotai_video_decoder_config_t;

typedef struct xiaotai_video_decoder {
    const char *name;
    xiaotai_video_load_t load;
    uint32_t flags;                   /* XIAOTAI_VIDEO_DECODER_* */
    xiaotai_video_pixel_t output;     /* layout it decodes to */
    size_t output_alignment;          /* of the buffer given to decode(); 0 or 1: none */
    uint32_t stack_bytes;             /* stack its task needs */
    bool stack_internal;              /* the stack must be XIAOTAI_MEM_INTERNAL (the driver runs with the flash cache off) */
    /* 0: cannot decode this; higher is better. Must not allocate. */
    int (*match)(const xiaotai_video_decoder_config_t *config);
    /* One open decoder is one reference chain: after a lost frame the engine
     * closes it and opens another at the next key frame (unless PERSISTENT). */
    xiaotai_err_t (*open)(const xiaotai_video_decoder_config_t *config, void **state);
    /* Decode one frame into out (room for max_width x max_height with both
     * sides rounded up to 16, in the output layout). On success *image
     * describes the picture in out, including its stride; a picture larger
     * than the maximum is cut to its centre max_width x max_height where the
     * codec allows cropping.
     *   XIAOTAI_ERR_NOT_FOUND      consumed, no picture came out (headers only)
     *   XIAOTAI_ERR_NOT_SUPPORTED  a format this decoder does not handle; not transient
     *   XIAOTAI_ERR_INVALID_SIZE   the picture exceeds the maximum and cannot be
     *                              cropped, or is smaller than the codec's rules allow
     *   XIAOTAI_ERR_TIMEOUT        the hardware did not finish in time
     *   anything else              the frame is lost; the reference chain is broken
     * The call may take long; the engine watches it from another task and stops
     * accepting frames while a call has not returned for 2 s. */
    xiaotai_err_t (*decode)(void *state, const xiaotai_video_packet_t *packet, uint8_t *out,
                            size_t capacity, xiaotai_video_image_t *image);
    void (*close)(void *state);
    /* Alternative to decode(), for a persistent resource that needs the
     * active downlink's limits before it can decode. The same input and
     * output rules apply. The config is borrowed for this call only. */
    xiaotai_err_t (*decode_configured)(void *state, const xiaotai_video_decoder_config_t *config,
                                      const xiaotai_video_packet_t *packet, uint8_t *out,
                                      size_t capacity, xiaotai_video_image_t *image);
} xiaotai_video_decoder_t;

/* --------------------------------------------------------------- transforms */

/* One geometric and pixel conversion, planned by the engine and executed
 * exactly by a backend: take `crop` out of the input, turn it, scale it to the
 * size of `place` and write it there in the output layout. The engine plans
 * crop and place from the backend's scale limits, so backends never round. */
typedef struct {
    xiaotai_video_pixel_t in_pixel;
    uint16_t in_width, in_height;
    uint16_t in_stride;                  /* pixels between row starts, >= in_width */
    bool in_full_range;
    xiaotai_video_rect_t crop;           /* inside the input; even position and size */
    xiaotai_video_rotation_t rotation;   /* applied to the cropped part */
    xiaotai_video_pixel_t out_pixel;
    uint16_t out_width, out_height;      /* the output buffer's picture, tightly packed */
    xiaotai_video_rect_t place;          /* inside the output; even position and size */
    bool fill;                           /* write black outside place */
} xiaotai_video_transform_config_t;

/* Scale factors a backend can apply exactly for a pair of layouts: n / denominator
 * with n a multiple of step and step <= n <= max. denominator 0: any ratio. */
typedef struct {
    uint16_t denominator;
    uint16_t step;
    uint16_t max;
    /* Set by the engine where no accelerator converts: a whole picture less
     * than a third too small for its area keeps its own size. */
    bool unenlarged;
} xiaotai_video_scale_limits_t;

typedef struct xiaotai_video_transform {
    const char *name;
    xiaotai_video_load_t load;
    size_t output_alignment;          /* of the buffer given to run(); 0 or 1: none */
    /* 0: cannot do exactly this; higher is better. Must not allocate. */
    int (*match)(const xiaotai_video_transform_config_t *config);
    /* False when it cannot convert between these layouts at all. */
    bool (*limits)(xiaotai_video_pixel_t in_pixel, xiaotai_video_pixel_t out_pixel,
                   xiaotai_video_scale_limits_t *limits);
    /* An open transform may be kept idle for any time and across sessions. */
    xiaotai_err_t (*open)(const xiaotai_video_transform_config_t *config, void **state);
    /* in: the input picture as configured, not modified. out: the whole output
     * picture (xiaotai_video_image_bytes of it, rounded up to the alignment).
     * Any failure leaves out undefined; the engine then tries the next best
     * backend for the same config. */
    xiaotai_err_t (*run)(void *state, const uint8_t *in, size_t in_size, uint8_t *out, size_t out_capacity);
    void (*close)(void *state);
    /* Optional: apply a different plan using only resources taken in open.
     * XIAOTAI_ERR_NOT_SUPPORTED when it cannot fit (the old plan stays valid);
     * the engine can use another backend for this frame without reallocating. */
    xiaotai_err_t (*reconfigure)(void *state, const xiaotai_video_transform_config_t *config);
} xiaotai_video_transform_t;

/* ----------------------------------------------------------------- registry */

/* [init] Add a backend; registration order breaks equal scores (first wins).
 * Also callable from xiaotai_video_port_init() and, in host tests, any time
 * before the engine starts. The first registration of a kind allocates that
 * kind's table (XIAOTAI_VIDEO_BACKENDS_MAX pointers, XIAOTAI_MEM_BULK).
 *   XIAOTAI_ERR_INVALID_ARG    NULL, no name, or a missing required function
 *   XIAOTAI_ERR_INVALID_STATE  the engine has started
 *   XIAOTAI_ERR_NO_MEM         XIAOTAI_VIDEO_BACKENDS_MAX of this kind exist,
 *                              or the table could not be allocated */
xiaotai_err_t xiaotai_video_encoder_register(const xiaotai_video_encoder_t *encoder);
xiaotai_err_t xiaotai_video_decoder_register(const xiaotai_video_decoder_t *decoder);
xiaotai_err_t xiaotai_video_transform_register(const xiaotai_video_transform_t *transform);

/* [any] The best backend for a config; NULL when none matches. `after` skips
 * backends up to and including that one in ranking order (NULL: the best), so
 * callers can fall back to the next. */
const xiaotai_video_encoder_t *xiaotai_video_encoder_find(const xiaotai_video_encoder_config_t *config);
const xiaotai_video_decoder_t *xiaotai_video_decoder_find(const xiaotai_video_decoder_config_t *config);
const xiaotai_video_transform_t *xiaotai_video_transform_find(const xiaotai_video_transform_config_t *config,
                                                              const xiaotai_video_transform_t *after);

/* [init] The platform half of this component: registers the backends the
 * platform has. Implemented once in esp-idf/ and once in host/ (which registers
 * none: the engine's own software transform is always present, and tests add
 * fakes). Called by xiaotai_engine_video_init(), not by anyone else. */
xiaotai_err_t xiaotai_video_port_init(void);

#ifdef __cplusplus
}
#endif
