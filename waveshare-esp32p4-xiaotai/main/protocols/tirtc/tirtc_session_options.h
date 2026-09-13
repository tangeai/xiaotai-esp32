#pragma once

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "media_tuning.h"

#define TIRTC_SESSION_CONTROL_EVENT_WAIT_TICKS  pdMS_TO_TICKS(20)
#define TIRTC_SESSION_TEARDOWN_EVENT_WAIT_TICKS pdMS_TO_TICKS(100)

#define TIRTC_SESSION_WORKER_TASK_STACK    (32 * 1024)
#define TIRTC_SESSION_WORKER_TASK_PRIORITY 3
#define TIRTC_SESSION_WORKER_POLL_MS       100U
#ifndef TIRTC_SESSION_WORKER_STACK_INTERNAL
#define TIRTC_SESSION_WORKER_STACK_INTERNAL 0
#endif
#define TIRTC_SESSION_TEARDOWN_TASK_STACK  (8 * 1024)
#define TIRTC_SESSION_TEARDOWN_TASK_PRIORITY (TIRTC_SESSION_WORKER_TASK_PRIORITY + 1)
#define TIRTC_SESSION_SDK_API_LOCK_WAIT_MS 500U
#define TIRTC_SESSION_SDK_API_LOCK_WAIT_TICKS pdMS_TO_TICKS(TIRTC_SESSION_SDK_API_LOCK_WAIT_MS)
/*
 * ThingConnect issues a short-lived one-time token for every active call.
 * Reusing SDK-cached parameters can bind a new room to stale signaling state,
 * so retries are owned by the call state machine and always fetch a new token.
 */
#define TIRTC_SESSION_CONNECT_CACHE_ENABLE 0

#define TIRTC_SESSION_MAX_SEND_BUFFER                   (2U * 1024U * 1024U)
#define TIRTC_SESSION_SEND_BUFFER_WARN_PCT              60U
/* Leave control/heartbeat headroom when ESP-Hosted is recovering from a DMA stall. */
#define TIRTC_SESSION_SEND_BUFFER_VIDEO_THROTTLE_PCT    70U
#define TIRTC_SESSION_SEND_BUFFER_DROP_PCT              85U
#define TIRTC_SESSION_SEND_BUFFER_LOG_PERIOD_MS         1000U
/* Sample outside the realtime tasks, but serialize the connection query with
 * every other SDK call. Two samples per second keep the 4 Mbit/s worst-case
 * growth below the throttle-to-drop margin without adding a 5 Hz SDK query to
 * the media hot path. */
#define TIRTC_SESSION_SEND_BUFFER_QUERY_PERIOD_MS       500U

/*
 * Keep enough PSRAM-backed slots to absorb an IDR send burst without dropping
 * an arbitrary H264 P-frame. H264 continuity is recovered with a fresh IDR
 * instead of the JPEG-style "drop oldest and keep latest" policy.
 */
#define TIRTC_SESSION_VIDEO_TX_QUEUE_LEN                4
#define TIRTC_SESSION_VIDEO_TX_TARGET_BACKLOG           2
#define TIRTC_SESSION_VIDEO_TX_BUFFER_POOL_SIZE         (TIRTC_SESSION_VIDEO_TX_QUEUE_LEN + 2)
#define TIRTC_SESSION_VIDEO_TX_BUFFER_SLOT_INVALID      UINT8_MAX
/*
 * Reserve every uplink slot at the encoder's maximum single-frame capacity.
 * P4 has enough PSRAM for the fixed pool; growing a slot during a key-frame or
 * high-motion burst adds allocator latency exactly where the send path is most
 * sensitive.
 */
#define TIRTC_SESSION_VIDEO_TX_PREALLOC_BYTES           APP_MEDIA_H264_OUTPUT_BUFFER_BYTES
#define TIRTC_SESSION_VIDEO_TX_ALLOC_ALIGN_BYTES        (64U * 1024U)
#define TIRTC_SESSION_VIDEO_TX_TASK_STACK               (8 * 1024)
/*
 * Audio and downlink decode keep realtime precedence on CPU0. Video freshness
 * is measured from queue admission, so uplink capture does not need to compete
 * at the same priority merely because capture/encode work took longer.
 */
#define TIRTC_SESSION_VIDEO_TX_TASK_PRIORITY            15
#define TIRTC_SESSION_VIDEO_TX_SDK_API_LOCK_WAIT_MS     40U
#define TIRTC_SESSION_VIDEO_TX_SDK_API_LOCK_WAIT_TICKS  pdMS_TO_TICKS(TIRTC_SESSION_VIDEO_TX_SDK_API_LOCK_WAIT_MS)
#define TIRTC_SESSION_VIDEO_TX_MAX_AGE_US               600000ULL
#define TIRTC_SESSION_VIDEO_TX_ISSUE_LOG_PERIOD_MS      1000U
/*
 * Keep normal media logging quiet, but expose a blocking SDK send call that
 * can starve camera cadence. The five-second limiter keeps this diagnostic
 * usable during a prolonged weak-network interval without loading UART.
 */
#define TIRTC_SESSION_VIDEO_TX_STALL_US                  100000ULL
#define TIRTC_SESSION_VIDEO_TX_STALL_LOG_INTERVAL_US     5000000ULL
/* The stable 15 fps profile should advance every 67 ms. Treat 2.5 s without
 * one accepted frame as a real pipeline stall, then emit one compact line per
 * 10 s. */
#define TIRTC_SESSION_VIDEO_TX_LIVENESS_TIMEOUT_US       2500000ULL
#define TIRTC_SESSION_VIDEO_TX_LIVENESS_LOG_INTERVAL_US  10000000ULL
/* A video call should produce its first packet promptly and then keep packets
 * flowing even for a static scene. After three silent seconds, reassert the
 * protocol subscription at a deliberately slow cadence instead of tearing
 * down an otherwise healthy P2P connection. */
#define TIRTC_SESSION_VIDEO_RX_LIVENESS_TIMEOUT_US        3000000ULL
#define TIRTC_SESSION_VIDEO_RX_RECOVERY_INTERVAL_US       8000000ULL
#define TIRTC_SESSION_VIDEO_RX_LIVENESS_LOG_INTERVAL_US  10000000ULL
/*
 * The audio queue is an overload reserve, not a latency budget. Keep at most
 * two 20 ms capture frames pending and discard speech that is already too old
 * to remain conversational.
 */
#define TIRTC_SESSION_AUDIO_TX_QUEUE_LEN                16
#define TIRTC_SESSION_AUDIO_TX_TARGET_BACKLOG           2
#define TIRTC_SESSION_AUDIO_TX_BUFFER_POOL_SIZE         (TIRTC_SESSION_AUDIO_TX_QUEUE_LEN + 2)
#define TIRTC_SESSION_AUDIO_TX_BUFFER_BYTES             (8U * 1024U)
#define TIRTC_SESSION_AUDIO_TX_BUFFER_SLOT_INVALID      UINT8_MAX
#define TIRTC_SESSION_AUDIO_TX_TASK_STACK               (5 * 1024)
#define TIRTC_SESSION_AUDIO_TX_TASK_PRIORITY            16
#define TIRTC_SESSION_AUDIO_TX_MAX_AGE_US               120000ULL

#define TIRTC_SESSION_ENABLE_TIME_STREAM_MESSAGES       0
#define TIRTC_SESSION_TIME_MESSAGE_INITIAL_DELAY_US     250000ULL
#define TIRTC_SESSION_TIME_MESSAGE_RETRY_DELAY_US       50000ULL
#define TIRTC_SESSION_TIME_MESSAGE_PERIOD_US            10000000ULL

#ifndef TIRTC_AUTO_PUSH_LOCAL_MEDIA_ON_CONNECT
#define TIRTC_AUTO_PUSH_LOCAL_MEDIA_ON_CONNECT           1
#endif
#define TIRTC_SESSION_DEFAULT_AUTO_MEDIA                 (TIRTC_AUTO_PUSH_LOCAL_MEDIA_ON_CONNECT != 0)
#ifndef TIRTC_SESSION_VIDEO_FIRST_DEFER_AUDIO
#define TIRTC_SESSION_VIDEO_FIRST_DEFER_AUDIO            1
#endif

#define TIRTC_SESSION_DEFERRED_FULL_RESET_DELAY_US      200000ULL
#define TIRTC_SESSION_RESTART_AFTER_FULL_RESET_DELAY_US 300000ULL
#define TIRTC_SESSION_SDK_RESTART_SETTLE_US             1500000ULL
#define TIRTC_SESSION_MEDIA_BOOTSTRAP_INITIAL_DELAY_US  5000ULL
#define TIRTC_SESSION_MEDIA_BOOTSTRAP_RETRY_DELAY_US    5000ULL
#define TIRTC_SESSION_MEDIA_AUDIO_FOLLOWUP_DELAY_US     10000ULL
/*
 * The device-side accepted callback can precede the underlying peer
 * connection's CONNECTED state. Match the SDK's 10-second ICE/connect window
 * so a slower relay path is not torn down by an early media send.
 */
#define TIRTC_SESSION_INVALID_HANDLE_GRACE_US          12000000ULL
#define TIRTC_SESSION_TEST_MEDIA_WARMUP_US              80000ULL
#define TIRTC_SESSION_TEST_MEDIA_RETRY_DELAY_US         40000ULL
#define TIRTC_SESSION_DISCONNECT_TIMEOUT_US             3000000ULL
#define TIRTC_SESSION_START_RETRY_DELAY_US              10000000ULL
#define TIRTC_SESSION_STOP_WAIT_MS                      1000U
#define TIRTC_SESSION_REMOTE_CLOSE_WARN_AGE_MS          5000U

#define TIRTC_SESSION_SDK_LOG_LEVEL                     0
#define TIRTC_SESSION_WEBRTC_LOG_LEVEL                  (TIRTC_SESSION_SDK_LOG_LEVEL > 10 ? (TIRTC_SESSION_SDK_LOG_LEVEL - 10) : 0)
#define TIRTC_SESSION_SDK_LOG_CHUNK_LEN                 160

#define TIRTC_SESSION_CONN_USER_MAGIC                   0x52544353U
