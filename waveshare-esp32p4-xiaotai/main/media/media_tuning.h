#pragma once

/*
 * Product media tuning.
 *
 * Keep values that are adjusted during camera, transport, and call tuning in
 * this file. Kconfig is reserved for build composition and hardware feature
 * switches; generated sdkconfig files must not become the source of truth for
 * runtime media policy.
 */

/* Normal RTC/IPC uplink profile. */
#define APP_MEDIA_CAMERA_CAPTURE_WIDTH                  1280U
#define APP_MEDIA_CAMERA_CAPTURE_HEIGHT                 960U
#define APP_MEDIA_RTC_VIDEO_WIDTH                       1280U
#define APP_MEDIA_RTC_VIDEO_HEIGHT                      960U
#define APP_MEDIA_RTC_H264_BITRATE_BPS                  4000000U
#define APP_MEDIA_RTC_H264_FPS                          20U
#define APP_MEDIA_RTC_H264_MIN_QP                       30U
#define APP_MEDIA_RTC_H264_MAX_QP                       45U

/*
 * Stable full-duplex device-call profile.
 *
 * The encoder bitrate is a rate-control target, not a hard ceiling. Use the
 * 4:3 landscape surface with 384 macroblocks per frame keeps the P4 software
 * decoder inside the frame budget under full-frame motion. The receiver scales
 * once to the panel-native 480x320 surface through PPA. Keep the same per-frame
 * bit budget as the validated 320 kbit/s at 15 fps profile while reserving CPU
 * time for full-duplex audio, UI, and transport. Let rate control
 * use the full legal H.264 QP range when full-frame motion would otherwise
 * exceed the peer's software-decode budget. Weak-network adaptation remains
 * opt-in and starts from this normal profile.
 */
#define APP_MEDIA_CALL_VIDEO_WIDTH                      384U
#define APP_MEDIA_CALL_VIDEO_HEIGHT                     256U
#define APP_MEDIA_CALL_VIDEO_FPS                        12U
#define APP_MEDIA_CALL_VIDEO_BITRATE_BPS                256000U
#define APP_MEDIA_CALL_VIDEO_MIN_QP                     34U
#define APP_MEDIA_CALL_VIDEO_MAX_QP                     51U

/* XiaoTai phone/browser uplink: user-selected full-frame 4:3. Remote hardware/software
 * decodes this stream; P4-to-P4's software decoder budget is a separate policy.
 * 15 fps / 2 Mbps is the initial product target, not a hardware ceiling. */
/* Full sensor frame after board mounting correction (counterclockwise 90). */
#define APP_MEDIA_WECHAT_VIDEO_WIDTH                    960U
#define APP_MEDIA_WECHAT_VIDEO_HEIGHT                   1280U
#define APP_MEDIA_WECHAT_VIDEO_FPS                      15U
#define APP_MEDIA_WECHAT_VIDEO_BITRATE_BPS              2000000U
#define APP_MEDIA_WECHAT_VIDEO_MIN_QP                   30U
#define APP_MEDIA_WECHAT_VIDEO_MAX_QP                   46U
/* H264 encoder and transport protection. */
/*
 * IPC keeps a two-second recovery interval. Full-duplex calls use a longer
 * sixteen-second interval because moving 480x320 IDR frames take the software
 * P4 decoder roughly two frame budgets while delta frames remain near budget.
 * Stream start, subscription, transport recovery, and peer requests still
 * force an immediate IDR, so the longer nominal GOP does not replace explicit
 * recovery signaling.
 */
#define APP_MEDIA_H264_GOP_DURATION_MS                  2000U
#define APP_MEDIA_CALL_H264_GOP_DURATION_MS             16000U
#define APP_MEDIA_H264_OUTPUT_BUFFER_BYTES              (1024U * 1024U)
#define APP_MEDIA_H264_MAX_DELTA_PAYLOAD_BYTES          (256U * 1024U)
#define APP_MEDIA_H264_STARTUP_GUARD_MS                 2500U
#define APP_MEDIA_H264_STARTUP_MAX_DELTA_PAYLOAD_BYTES  (128U * 1024U)
#define APP_MEDIA_H264_KEY_FRAME_REQUEST_MIN_INTERVAL_MS 2000U
#define APP_MEDIA_TRANSPORT_BACKPRESSURE_HOLD_MS        80U

/* Local pressure fallback. Disabled builds do not execute this policy. */
#define APP_MEDIA_AUTO_DEGRADE_SAMPLES                  2U
#define APP_MEDIA_AUTO_RECOVER_SAMPLES                  6U
#define APP_MEDIA_AUTO_COOLDOWN_MS                      10000U
#define APP_MEDIA_AUTO_PRESSURE_BUFFER_PCT              8U
#define APP_MEDIA_AUTO_SEVERE_BUFFER_PCT                25U
#define APP_MEDIA_AUTO_HEALTHY_BUFFER_PCT               2U
#define APP_MEDIA_AUTO_PRESSURE_QUEUE_DEPTH             2U
#define APP_MEDIA_AUTO_SEVERE_QUEUE_DEPTH               4U

/* TGMP bitrate controller hysteresis. */
#define APP_MEDIA_TGMP_EVENT_MIN_INTERVAL_US            500000ULL
#define APP_MEDIA_TGMP_EVENT_FAST_STEP_BPS              64000U
/* Keep the normal device-call profile at 256 kbit/s. The lower bound is only
 * exposed to TGMP after transport feedback reports congestion. At 160 kbit/s
 * the measured 300 ms / 5% loss path still accumulated retransmission bursts
 * large enough to overflow the P4 decoder ingress queue; 96 kbit/s leaves the
 * controller one real protection step without degrading a healthy call. */
#define APP_MEDIA_TGMP_COMPACT_MIN_BITRATE_BPS           (96U * 1000U)
#define APP_MEDIA_TGMP_LARGE_MIN_BITRATE_BPS            (750U * 1000U)
#define APP_MEDIA_TGMP_MIN_RATIO_DIVISOR                4U
#define APP_MEDIA_TGMP_START_RANGE_PERCENT              80U
#define APP_MEDIA_TGMP_MIN_STEP_BPS                     (16U * 1000U)
#define APP_MEDIA_TGMP_MIN_STEP_PERCENT                 10U
#define APP_MEDIA_TGMP_PROTECTION_INTERVAL_MS           1000U
#define APP_MEDIA_TGMP_EMERGENCY_DROP_PERCENT           25U
#define APP_MEDIA_TGMP_RECOVERY_HOLD_MS                 5000U
#define APP_MEDIA_TGMP_RECOVERY_INTERVAL_MS             3000U
#define APP_MEDIA_TGMP_RECOVERY_STEP_PERCENT            15U
#define APP_MEDIA_TGMP_RECOVERY_MIN_STEP_BPS            (64U * 1000U)

/* Focused diagnostics. Keep the initial trace count at zero for normal use. */
#define APP_MEDIA_CAMERA_FRAME_TRACE_INITIAL_COUNT      0U
#define APP_MEDIA_CAMERA_FRAME_TRACE_INTERVAL_MS        10000U

#if APP_MEDIA_CALL_VIDEO_MIN_QP > APP_MEDIA_CALL_VIDEO_MAX_QP
#error "Call video minimum QP must not exceed maximum QP"
#endif

#if APP_MEDIA_CALL_VIDEO_FPS == 0U || APP_MEDIA_CALL_VIDEO_FPS > 15U
#error "Stable call profile must stay within 1-15 fps"
#endif

#if APP_MEDIA_RTC_H264_MIN_QP > APP_MEDIA_RTC_H264_MAX_QP
#error "RTC video minimum QP must not exceed maximum QP"
#endif

#if (APP_MEDIA_CALL_VIDEO_WIDTH % 16U) != 0U || \
    (APP_MEDIA_CALL_VIDEO_HEIGHT % 16U) != 0U
#error "Call video dimensions must be aligned to 16 pixels"
#endif

#if (APP_MEDIA_WECHAT_VIDEO_WIDTH % 16U) != 0U || \
    (APP_MEDIA_WECHAT_VIDEO_HEIGHT % 16U) != 0U
#error "WeChat video dimensions must be aligned to 16 pixels"
#endif

#if APP_MEDIA_TGMP_COMPACT_MIN_BITRATE_BPS > APP_MEDIA_CALL_VIDEO_BITRATE_BPS
#error "TGMP compact floor must not exceed the normal call bitrate"
#endif

#if APP_MEDIA_TGMP_START_RANGE_PERCENT == 0U || \
    APP_MEDIA_TGMP_START_RANGE_PERCENT >= 100U
#error "TGMP start range percentage must stay strictly between 0 and 100"
#endif
