// SPDX-License-Identifier: MIT
#pragma once
/* Internal constants of the video engine. Values a product chooses are build
 * options (xiaotai/video_config.h); values a session chooses are profile or
 * downlink fields (xiaotai/video.h). What is here is fixed by the engine's
 * design and by the measurements its specification cites. */
#include "xiaotai/video_config.h"

/* ------------------------------------------------------------------ tasks */

/* Priorities. Hard order the product keeps with the media engine's tuning:
 * every audio task > hardware codec tasks (XIAOTAI_VIDEO_HARDWARE_TASK_PRIORITY)
 * > relay and convert > software encoding; the session task and software
 * decoding sit at 5, the UI at 6. */
#define VIDEO_SESSION_PRIORITY 5U
#define VIDEO_SOFTWARE_UPLINK_PRIORITY 3U
#define VIDEO_SOFTWARE_DECODE_PRIORITY 5U
#define VIDEO_RELAY_PRIORITY 4U
#define VIDEO_CONVERT_PRIORITY 4U

/* Stacks; all of them XIAOTAI_MEM_BULK except a decoder that asks for internal
 * memory (its driver runs with the flash cache off). */
#define VIDEO_SESSION_STACK_BYTES (8U * 1024U)
#define VIDEO_UPLINK_STACK_BYTES (16U * 1024U)
#define VIDEO_RELAY_STACK_BYTES (3U * 1024U)
#define VIDEO_CONVERT_STACK_BYTES (8U * 1024U)
/* Used when a decoder descriptor leaves stack_bytes 0. */
#define VIDEO_SOFTWARE_DECODE_STACK_BYTES (16U * 1024U)
#define VIDEO_HARDWARE_DECODE_STACK_BYTES (8U * 1024U)

/* Cores. The session and convert tasks stay off the core that runs the
 * network stack's control work; the software decoder is never pinned: on core
 * 0 it starves MQTT and HTTP, on core 1 it piles up delay. Without a pixel
 * accelerator conversion is computing too, and beside echo cancellation and
 * the decoder one core is too little. Only a hint on single-core chips. */
#define VIDEO_SESSION_CORE 1
#if defined(XIAOTAI_VIDEO_PIXEL_ACCELERATOR) && XIAOTAI_VIDEO_ACCELERATORS
#define VIDEO_CONVERT_CORE 1
#else
#define VIDEO_CONVERT_CORE XIAOTAI_CORE_ANY
#endif

/* Memory taken per session (xiaotai_video_config_t.downlink_on_demand): what
 * stays free beside it, for the decoder's own pictures and the call; after how
 * many tries half a second apart the next ones come slower; how many decodes
 * in a row without memory end the picture, and how long until it is tried again. */
#define VIDEO_ON_DEMAND_RESERVE (384U * 1024U)
#define VIDEO_ON_DEMAND_QUICK_TRIES 8U
#define VIDEO_ON_DEMAND_SLOW_MS 5000U
#define VIDEO_DECODER_STARVED 2U
#define VIDEO_DECODER_STARVED_MS 10000U

/* How long xiaotai_video_reset() waits for each engine task to leave. */
#define VIDEO_TASK_EXIT_WAIT_MS 5000U

/* ------------------------------------------------- uplink normalization */

/* H.264 profiles are normalized when declared, as the governor of the
 * existing pipeline did for every configuration it accepted: at least
 * 320 x 240, even sides, 5..30 fps, 200 kbit/s..12 Mbit/s, QP 10..51 with the
 * pipeline's defaults (30, 45) for a bound that is out of range. */
#define VIDEO_H264_MIN_WIDTH 320U
#define VIDEO_H264_MIN_HEIGHT 240U
#define VIDEO_H264_MIN_FPS 5U
#define VIDEO_H264_MAX_FPS 30U
#define VIDEO_H264_MIN_BITRATE_BPS (200U * 1000U)
#define VIDEO_H264_MAX_BITRATE_BPS (12U * 1000U * 1000U)
#define VIDEO_H264_QP_LOWEST 10U
#define VIDEO_H264_QP_HIGHEST 51U
#define VIDEO_H264_DEFAULT_MIN_QP 30U
#define VIDEO_H264_DEFAULT_MAX_QP 45U

/* JPEG profiles are checked, not normalized. */
#define VIDEO_JPEG_MIN_FPS 1U
#define VIDEO_JPEG_MAX_FPS 30U
#define VIDEO_JPEG_MAX_QUALITY 100U

/* ------------------------------------------------------------ viewports */

/* Finished frames of a viewport: one being written, one ready, one shown at
 * least; the default is 20 for a buffered downlink and 3 otherwise. */
#define VIDEO_VIEWPORT_MIN_FRAMES 3U
/* ... and with a product's limit: one shown, and one that is written and then
 * ready (the next picture is written over one that was never shown). */
#define VIDEO_VIEWPORT_LIMITED_FRAMES 2U
/* Received frames that wait for a decoder, at least. */
#define VIDEO_DOWNLINK_MIN_PACKETS 4U
#define VIDEO_VIEWPORT_BUFFERED_FRAMES 20U
#define VIDEO_VIEWPORT_PLAIN_FRAMES 3U

/* Downlink descriptions are kept one per video codec of xiaotai/codec.h. */
#define VIDEO_DOWNLINK_CODECS 2U

/* ---------------------------------------------------------- event log */

#define VIDEO_EVENTLOG_SOURCE "video"
