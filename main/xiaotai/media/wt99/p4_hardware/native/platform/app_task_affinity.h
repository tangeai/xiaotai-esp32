#pragma once

#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#include "app_memory_policy.h"

/*
 * ESP32-P4 runtime ownership:
 * - CPU0 owns network/control deadlines and CPU1 owns UI/audio deadlines.
 *   TinyH264 is SMP-migratable below both classes: it can consume an idle CPU,
 *   while Wi-Fi/RTC, audio, conversion and LVGL preempt it when their deadlines
 *   arrive. Pinning the decoder to CPU0 starved MQTT/HTTP; pinning it to CPU1
 *   made high-motion streams accumulate compressed-frame latency.
 * - The actual capture/AEC, Opus TX and playback workers outrank hardware
 *   video completion and UI. Keep policy bound to their task creation sites.
 * - Camera uplink outranks display and TinyH264 decode. This keeps the cadence
 *   stable without letting a complex decode monopolize CPU1 and delay the
 *   downlink conversion or touch/UI owner.
 * - The application video-TX worker is also SMP-migratable. Its SDK call can
 *   overlap a long decode window, so pinning it to CPU0 turns normal scheduler
 *   preemption into artificial send latency and leaves less time for transport
 *   receive processing. RTC control remains on CPU0. Audio TX is explicitly
 *   on the audio core before DSP can pin an unbound task through lazy FPU use.
 * - TinyH264 remains single-task. Its optional dual-task filter worker is
 *   disabled because dynamic calls reproduced a race inside the prebuilt
 *   decoder. Core separation uses both CPUs without entering that path.
 *
 * Keep task creation sites using these names instead of raw core numbers so the
 * scheduling contract stays visible when modules are moved or added.
 */
#define APP_TASK_PRIORITY_AUDIO_CAPTURE   8U
#define APP_TASK_PRIORITY_AUDIO_PLAYBACK  8U
#define APP_TASK_PRIORITY_AUDIO_PACKET    8U
#define APP_TASK_PRIORITY_CAMERA          7U
#define APP_TASK_PRIORITY_MJPEG           7U
#define APP_TASK_PRIORITY_UI              6U

#if CONFIG_FREERTOS_UNICORE
#define APP_TASK_CORE_UI         tskNO_AFFINITY
#define APP_TASK_CORE_AUDIO      tskNO_AFFINITY
#define APP_TASK_CORE_NETWORK    tskNO_AFFINITY
#define APP_TASK_CORE_RTC        tskNO_AFFINITY
#define APP_TASK_CORE_RTC_VIDEO_TX tskNO_AFFINITY
#define APP_TASK_CORE_CAMERA     tskNO_AFFINITY
#define APP_TASK_CORE_BACKGROUND tskNO_AFFINITY
#define APP_TASK_CORE_VIDEO_DECODE  tskNO_AFFINITY
#define APP_TASK_CORE_VIDEO_CONVERT tskNO_AFFINITY
#else
#define APP_TASK_CORE_UI         1
#define APP_TASK_CORE_AUDIO      1
#define APP_TASK_CORE_NETWORK    0
#define APP_TASK_CORE_RTC        0
#define APP_TASK_CORE_RTC_VIDEO_TX tskNO_AFFINITY
#define APP_TASK_CORE_CAMERA     tskNO_AFFINITY
#define APP_TASK_CORE_BACKGROUND 0
#define APP_TASK_CORE_VIDEO_DECODE  tskNO_AFFINITY
#define APP_TASK_CORE_VIDEO_CONVERT 1
#endif
