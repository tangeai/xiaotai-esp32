// SPDX-License-Identifier: MIT
#pragma once
/* Video engine: build-time options (xiaotai/config.h). On ESP-IDF they are set
 * in the component's Kconfig; the defaults here equal the Kconfig defaults.
 * These are choices of a product that are neither a part descriptor nor a
 * session profile. A new option is one more entry here and in Kconfig. Third-
 * party options the engine's ESP-IDF backends rely on are in the component's
 * sdkconfig.defaults files (one per target that has those libraries). */
#include "xiaotai/config.h"

/* Registry capacities. Each registry is a table allocated from
 * XIAOTAI_MEM_BULK by its first registration (in an init(), where allocating
 * is allowed), so a product that declares nothing holds no table. Registering
 * beyond a capacity, or when the table cannot be allocated, is
 * XIAOTAI_ERR_NO_MEM in the registering init(), so the start fails loudly
 * instead of losing a profile, a viewport or a backend; a product that brings
 * more raises the option, and no header changes. */
/* Distinct uplink profiles xiaotai_video_require_uplink() keeps. Downlink
 * descriptions need no option: one per codec is kept. */
#ifdef CONFIG_XIAOTAI_VIDEO_PROFILES_MAX
#define XIAOTAI_VIDEO_PROFILES_MAX CONFIG_XIAOTAI_VIDEO_PROFILES_MAX
#else
#define XIAOTAI_VIDEO_PROFILES_MAX 8
#endif
/* Viewports xiaotai_video_require_viewport() keeps: one per display. */
#ifdef CONFIG_XIAOTAI_VIDEO_VIEWPORTS_MAX
#define XIAOTAI_VIDEO_VIEWPORTS_MAX CONFIG_XIAOTAI_VIDEO_VIEWPORTS_MAX
#else
#define XIAOTAI_VIDEO_VIEWPORTS_MAX 4
#endif
/* Backends of one kind (encoders, decoders or transforms) the registries of
 * xiaotai/video_backend.h keep; the engine's own software transform is one of
 * the transforms. */
#ifdef CONFIG_XIAOTAI_VIDEO_BACKENDS_MAX
#define XIAOTAI_VIDEO_BACKENDS_MAX CONFIG_XIAOTAI_VIDEO_BACKENDS_MAX
#else
#define XIAOTAI_VIDEO_BACKENDS_MAX 8
#endif

/* Draw the local time of day (HH:MM:SS.mmm; dashes while the clock is not
 * synchronized) into the top-left corner of every uplink picture that passes
 * through a transform, before it is encoded. Lets a bench measure end-to-end
 * delay by reading the picture. Pictures sent to the encoder straight from the
 * camera are never drawn on: camera frames are read-only loans. */
#define XIAOTAI_VIDEO_UPLINK_CLOCK_OVERLAY \
    XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_VIDEO_UPLINK_CLOCK_OVERLAY, 0)

/* Use the chip's pixel accelerator and hardware JPEG decoder. Off where an RGB
 * panel is scanned out of external memory: their DMA makes it lose pixels. */
#define XIAOTAI_VIDEO_ACCELERATORS XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_VIDEO_ACCELERATORS, 1)

/* Decode JPEG in software: for a chip without a JPEG decoder, or with one the
 * option above turns off. */
#define XIAOTAI_VIDEO_JPEG_SOFTWARE_DECODER XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_VIDEO_JPEG_SOFTWARE_DECODER, 0)

/* Encode JPEG in software. A product that sends no pictures can leave it out. */
#define XIAOTAI_VIDEO_JPEG_ENCODER XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_VIDEO_JPEG_ENCODER, 1)

/* Core of the camera task when its encoder is a software one (-1: any core).
 * A software encoder competes with echo cancellation for the cache of external
 * RAM; the default keeps it on the core that does not run audio processing. */
#ifdef CONFIG_XIAOTAI_VIDEO_SOFTWARE_UPLINK_CORE
#define XIAOTAI_VIDEO_SOFTWARE_UPLINK_CORE CONFIG_XIAOTAI_VIDEO_SOFTWARE_UPLINK_CORE
#else
#define XIAOTAI_VIDEO_SOFTWARE_UPLINK_CORE 0
#endif

/* Priority of the tasks whose backends load the CPU lightly
 * (XIAOTAI_VIDEO_LOAD_HARDWARE: the camera task with a hardware encoder, the
 * hardware JPEG decode task). Precondition the product keeps: strictly below
 * every audio task of the media engine (its task.*.priority tuning), so a
 * camera or decode burst never shares a time slice with audio capture or
 * playout. The default suits audio tasks at 8 or above; a product whose
 * audio runs at 7 sets 5. The engine's other tasks have fixed priorities. */
#ifdef CONFIG_XIAOTAI_VIDEO_HARDWARE_TASK_PRIORITY
#define XIAOTAI_VIDEO_HARDWARE_TASK_PRIORITY CONFIG_XIAOTAI_VIDEO_HARDWARE_TASK_PRIORITY
#else
#define XIAOTAI_VIDEO_HARDWARE_TASK_PRIORITY 7
#endif
