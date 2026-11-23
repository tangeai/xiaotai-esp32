// SPDX-License-Identifier: MIT
#pragma once
/* Tuned video profiles, as initializers. They are named for what the peer and
 * the purpose require, not for a module or a board; a module picks one for its
 * peer and passes it to xiaotai_video_require_uplink() and xiaotai_video_open().
 * The numbers were measured on the bench; change them here, in one place.
 *
 *   static const xiaotai_video_profile_t profile = XIAOTAI_VIDEO_PROFILE_H264_COMPACT;
 *
 * Every field is listed, in declaration order, so the initializers are valid
 * and warning-free in C and in C++. A new purpose is one more initializer; the
 * existing ones do not change. */
#include "xiaotai/video.h"

/* H.264 for a peer that decodes in software (another device): 384 macroblocks a
 * frame keep that decoder inside its frame time under full-frame motion. The
 * long key frame interval is safe because requests, new viewers and send
 * recovery force a key frame at once. Range offered to the link: 96 / 224 /
 * 256 kbit/s (min / start / max). */
#define XIAOTAI_VIDEO_PROFILE_H264_COMPACT \
    { \
        .codec = XIAOTAI_VIDEO_CODEC_H264, \
        .width = 384, .height = 256, .max_pixels = 0u, \
        .fallback_width = 0, .fallback_height = 0, \
        .upright = false, \
        .fps = 12, \
        .bitrate_bps = 256000u, .min_qp = 34, .max_qp = 51, \
        .key_interval_s = 16, \
        .quality = 0, .min_quality = 0, \
        .max_frame_bytes = 256u * 1024u, \
        .backlog_limit_bytes = 0u, \
        .max_age_ms = 0, \
        .pressure_limit = 0, \
        .refetch_stale = false, \
        .retry_ms = 500, \
    }

/* H.264 for a phone or a browser: the camera's whole picture, upright, at most
 * 720 x 960 pixels (larger pictures took a whole frame period to encode).
 * Range offered to the link: 750 / 1750 / 2000 kbit/s. */
#define XIAOTAI_VIDEO_PROFILE_H264_SENSOR \
    { \
        .codec = XIAOTAI_VIDEO_CODEC_H264, \
        .width = 0, .height = 0, .max_pixels = 720u * 960u, \
        .fallback_width = 0, .fallback_height = 0, \
        .upright = true, \
        .fps = 15, \
        .bitrate_bps = 2000000u, .min_qp = 26, .max_qp = 46, \
        .key_interval_s = 2, \
        .quality = 0, .min_quality = 0, \
        .max_frame_bytes = 256u * 1024u, \
        .backlog_limit_bytes = 0u, \
        .max_age_ms = 0, \
        .pressure_limit = 0, \
        .refetch_stale = false, \
        .retry_ms = 500, \
    }

/* JPEG stream for a web viewer over a link shared with audio: small pictures,
 * skipped rather than queued (32 KiB of backlog is the limit), so video never
 * delays audio. A failure ends the uplink for that session. */
#define XIAOTAI_VIDEO_PROFILE_JPEG_STREAM \
    { \
        .codec = XIAOTAI_VIDEO_CODEC_JPEG, \
        .width = 240, .height = 176, .max_pixels = 0u, \
        .fallback_width = 160, .fallback_height = 120, \
        .upright = false, \
        .fps = 12, \
        .bitrate_bps = 0u, .min_qp = 0, .max_qp = 0, \
        .key_interval_s = 0, \
        .quality = 45, .min_quality = 30, \
        .max_frame_bytes = 32u * 1024u, \
        .backlog_limit_bytes = 32u * 1024u, \
        .max_age_ms = 250, \
        .pressure_limit = 12, \
        .refetch_stale = false, \
        .retry_ms = 0, \
    }

/* JPEG stills for a vision model: one fresh picture a second. */
#define XIAOTAI_VIDEO_PROFILE_JPEG_STILL \
    { \
        .codec = XIAOTAI_VIDEO_CODEC_JPEG, \
        .width = 320, .height = 240, .max_pixels = 0u, \
        .fallback_width = 240, .fallback_height = 176, \
        .upright = false, \
        .fps = 1, \
        .bitrate_bps = 0u, .min_qp = 0, .max_qp = 0, \
        .key_interval_s = 0, \
        .quality = 45, .min_quality = 30, \
        .max_frame_bytes = 64u * 1024u, \
        .backlog_limit_bytes = 32u * 1024u, \
        .max_age_ms = 750, \
        .pressure_limit = 3, \
        .refetch_stale = true, \
        .retry_ms = 0, \
    }

/* What a peer sending XIAOTAI_VIDEO_PROFILE_H264_COMPACT delivers. Two seconds
 * of burst cover the retransmission stalls measured on a lossy link (24
 * encoded frames). The peer is asked for a key frame every second while the
 * stream runs: transport losses do not always surface as decode errors, and a
 * sender of the compact profile merges such requests into one per 2 s. */
#define XIAOTAI_VIDEO_DOWNLINK_H264_COMPACT \
    { \
        .codec = XIAOTAI_VIDEO_CODEC_H264, \
        .max_width = 384, .max_height = 256, \
        .fps = 12, \
        .max_frame_bytes = 256u * 1024u, \
        .burst_ms = 2000u, \
        .key_refresh_ms = 1000, \
        .recover_h264_profile = true, .preserve_h264_parameter_chain = true, \
    }

/* Independent JPEG frames within the given picture budget (what the device
 * profile asks the peer to send), in either orientation as the downlink
 * description defines it: only the newest frame is shown and the peer is
 * never asked for key frames. (640, 480) and (1024, 608) are the budgets in
 * use; a module takes them from its product configuration, never from the
 * display's size. */
#define XIAOTAI_VIDEO_DOWNLINK_JPEG(picture_width, picture_height) \
    { \
        .codec = XIAOTAI_VIDEO_CODEC_JPEG, \
        .max_width = (picture_width), .max_height = (picture_height), \
        .fps = 0, \
        .max_frame_bytes = 256u * 1024u, \
        .burst_ms = 0u, \
        .key_refresh_ms = 0, \
    }
