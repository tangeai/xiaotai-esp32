// SPDX-License-Identifier: MIT
#pragma once
/* Build-time options of the UI system (xiaotai/config.h explains the
 * mechanism; the options themselves are in the component's Kconfig).
 *
 * What differs per board is not here: the display's size, pixel format,
 * render mode and transfer budget are in its descriptor (xiaotai/hal.h), and
 * the theme is chosen from that descriptor. These options place the UI task
 * among the product's other tasks and choose which backends are built. Where
 * a Kconfig default depends on external RAM, this header holds the value
 * without it, which the host uses. */
#include "xiaotai/config.h"
#include "xiaotai/view_config.h"

/* Whether a display drawn by the FRAME backend gets a UI task:
 *   1  its own task, as every LVGL display has (options below).
 *   0  no task and no stack: owner-task timers (xiaotai_timer_start) run the
 *      work the task would do, one bounded step per timer, on an absolute
 *      schedule: render after a view changed (coalesced into one step),
 *      the theme's tick() delays, once a second while a timer or the clock
 *      is shown, the pointer every XIAOTAI_UI_POINTER_READ_MS where the
 *      display has touch, sleep timeout, injected input, capture and control
 *      requests. Nothing is armed while nothing is due. A periodic step that
 *      runs more than one period behind its due time restarts its schedule
 *      from now without catching up and is counted
 *      (xiaotai_ui_info_t.late_steps). Each step is an
 *      owner-task handler and stays within its stack budget
 *      (XIAOTAI_FRAMEWORK_HANDLER_STACK_BYTES), so only a theme that asks for
 *      at most XIAOTAI_UI_OWNER_THEME_STACK_BYTES (xiaotai/ui_theme.h) is run
 *      this way; a display whose theme asks for more gets its task anyway and
 *      reports it (xiaotai_ui_info_t.owner_timer false). The cost: a step's
 *      present() runs on the owner task and delays the next handler by as
 *      long as the transfer takes (a 128x64 one-bit frame over I2C: about
 *      25 ms), as the main loop of a product without external RAM does when
 *      it draws itself.
 * A display whose theme draws with LVGL always has its task (LVGL's timers
 * and the theme's 16 KiB stack need it). Kconfig default: 1 with external
 * RAM, 0 without (the internal RAM ledger of the boards without it). */
#define XIAOTAI_UI_TASK XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_UI_TASK, 0)

/* Priority of each display's UI task. It draws and reads the pointer; it must
 * stay below the audio capture and playout tasks. */
#ifdef CONFIG_XIAOTAI_UI_TASK_PRIORITY
#define XIAOTAI_UI_TASK_PRIORITY CONFIG_XIAOTAI_UI_TASK_PRIORITY
#else
#define XIAOTAI_UI_TASK_PRIORITY 6
#endif

/* Core the UI task is pinned to; -1: any (xiaotai/port/os.h). Products with
 * two cores keep it off the core their audio processing runs on. */
#ifdef CONFIG_XIAOTAI_UI_TASK_CORE
#define XIAOTAI_UI_TASK_CORE CONFIG_XIAOTAI_UI_TASK_CORE
#else
#define XIAOTAI_UI_TASK_CORE (-1)
#endif

/* Least stack of the UI task in bytes; the task gets the larger of this and
 * what its theme asks for (xiaotai_theme_t.stack_bytes). The stack is
 * XIAOTAI_MEM_BULK: in external RAM where the board has it. */
#ifdef CONFIG_XIAOTAI_UI_TASK_STACK_BYTES
#define XIAOTAI_UI_TASK_STACK_BYTES CONFIG_XIAOTAI_UI_TASK_STACK_BYTES
#else
#define XIAOTAI_UI_TASK_STACK_BYTES 4096
#endif

/* Period in ms at which the UI task reads the pointer of a display with
 * touch, awake or asleep (a touch must wake it). The read comes before any
 * video paint of the same loop, so video never delays touch. A product keeps
 * its own period here (one that let LVGL read touch at its refresh period
 * sets that period). The drawing refresh period is not here: it is LVGL's
 * own (LV_DEF_REFR_PERIOD), set by the product's configuration where it
 * differs from LVGL's default. */
#ifdef CONFIG_XIAOTAI_UI_POINTER_READ_MS
#define XIAOTAI_UI_POINTER_READ_MS CONFIG_XIAOTAI_UI_POINTER_READ_MS
#else
#define XIAOTAI_UI_POINTER_READ_MS 15
#endif

/* Builds the LVGL backend (colour themes need it). Off: only the whole-frame
 * backend exists and a theme that names LVGL fails to start. */
#define XIAOTAI_UI_LVGL XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_UI_LVGL, 0)

/* Frame-buffer scanout on ESP-IDF's RGB LCD engine (XIAOTAI_SCANOUT_RGB). Not
 * a user choice: the RGB panel part selects it, so boards without an RGB
 * panel carry no RGB event slots. The host has no scanout. */
#define XIAOTAI_UI_RGB_SCANOUT XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_UI_RGB_SCANOUT, 0)

/* Builds xiaotai_ui_capture() and the command AT+SCREEN. On the LVGL backend
 * a capture takes one more frame of XIAOTAI_MEM_BULK while it runs. Off: both
 * report XIAOTAI_ERR_NOT_SUPPORTED. */
#define XIAOTAI_UI_CAPTURE XIAOTAI_CONFIG_BOOL(CONFIG_XIAOTAI_UI_CAPTURE, 1)

/* Registry capacities. Each registry is a table of pointers allocated from
 * XIAOTAI_MEM_BULK when the engine initializes. Registering beyond a capacity
 * is XIAOTAI_ERR_NO_MEM in the registering init(), so the start fails loudly
 * instead of losing a theme, template or image; a product whose applications
 * bring more raises the option, and no header changes. */
#ifdef CONFIG_XIAOTAI_UI_THEMES_MAX
#define XIAOTAI_UI_THEMES_MAX CONFIG_XIAOTAI_UI_THEMES_MAX
#else
#define XIAOTAI_UI_THEMES_MAX 8
#endif
#ifdef CONFIG_XIAOTAI_UI_TEMPLATES_MAX
#define XIAOTAI_UI_TEMPLATES_MAX CONFIG_XIAOTAI_UI_TEMPLATES_MAX
#else
#define XIAOTAI_UI_TEMPLATES_MAX 16
#endif
#ifdef CONFIG_XIAOTAI_UI_IMAGES_MAX
#define XIAOTAI_UI_IMAGES_MAX CONFIG_XIAOTAI_UI_IMAGES_MAX
#else
#define XIAOTAI_UI_IMAGES_MAX 16
#endif
/* Direct painters per display (LVGL backend: the procedural face and the
 * video region take one each), kept in the display's backend state. */
#ifdef CONFIG_XIAOTAI_UI_PAINTERS_MAX
#define XIAOTAI_UI_PAINTERS_MAX CONFIG_XIAOTAI_UI_PAINTERS_MAX
#else
#define XIAOTAI_UI_PAINTERS_MAX 4
#endif
