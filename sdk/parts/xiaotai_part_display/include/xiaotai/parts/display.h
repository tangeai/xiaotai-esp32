// SPDX-License-Identifier: MIT
#pragma once
/* Parts library: the display part, assembled from a panel driver, an optional
 * touch driver and an optional backlight.
 *
 * A panel driver knows one controller on one bus (an SPI colour panel, an I2C
 * monochrome panel, a MIPI DSI panel); a touch driver knows one touch
 * controller. Both are small interfaces defined here and implemented by other
 * components of the parts library, so a new panel or touch controller is one
 * more driver and this file does not change. The display part owns what is
 * common to all of them: the order of power-up, complete rollback of a failed
 * start, the backlight, the touch filter and the descriptor.
 *
 * Start order in open(): power steps in order, then panel and touch (touch
 * first when the board says so), backlight left at 0. A failure undoes
 * everything started so far in reverse order and open() returns the failure;
 * when the undo itself fails the hardware is in an unknown state, the part
 * latches and every later open() returns XIAOTAI_ERR_INVALID_STATE.
 *
 * Calling tasks: create functions are [board] (the board's Init()). The
 * functions of a panel or touch driver are called only by the display part,
 * which is called by the engine that owns displays ([engine] in xiaotai/hal.h):
 * calls on one driver never overlap. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/hal.h"
#include "xiaotai/parts/bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------ panel driver */

typedef struct xiaotai_panel {
    /* Required. Brings up bus, controller and panel memory and leaves the
     * panel showing black with its output on. Fills handles->panel, panel_io,
     * framebuffers and the orientation flags; the display part fills
     * handles->touch. On failure the driver has released what it acquired. */
    xiaotai_err_t (*start)(void *self, xiaotai_display_handles_t *handles);
    /* Required. Releases everything start() acquired, in reverse order. An
     * error means something could not be released. */
    xiaotai_err_t (*stop)(void *self);
    /* Required. Same contract as xiaotai_display_t.present. */
    xiaotai_err_t (*present)(void *self, const xiaotai_rect_t *area, const void *pixels);
    /* The panel's own output switch, for panels without a backlight (OLED).
     * NULL: none. */
    xiaotai_err_t (*set_on)(void *self, bool on);
    void *self;
    /* Optional: a failed start left an acquired resource behind. */
    bool (*cleanup_failed)(void *self);
} xiaotai_panel_t;

/* ------------------------------------------------------------ touch driver */

typedef struct xiaotai_touch {
    /* Required. Resets and configures the controller. *native receives the
     * platform's handle for renderers that want it (ESP-IDF:
     * esp_lcd_touch_handle_t), or NULL. */
    xiaotai_err_t (*start)(void *self, void **native);
    /* Required. */
    xiaotai_err_t (*stop)(void *self);
    /* Required. Touched points in the panel's native frame, at most capacity;
     * *count 0 when released. One bus transfer at most except for GT911's
     * status read, optional data read, and acknowledgement write. GT911
     * keeps a finite per-wait I2C budget for its at most three transfers. */
    xiaotai_err_t (*read)(void *self, xiaotai_touch_point_t *points, size_t capacity, size_t *count);
    void *self;
    /* Optional: a failed start left an acquired resource behind. */
    bool (*cleanup_failed)(void *self);
} xiaotai_touch_t;

/* What an I2C touch controller needs from the board. The controller libraries
 * drive reset and interrupt themselves (some use them to select the address),
 * so these two are pins of the chip, not lines. */
typedef struct {
    xiaotai_i2c_bus_t *bus;
    uint8_t address;            /* 7-bit; 0: the controller's default */
    uint32_t hz;
    uint16_t width, height;     /* the panel's native frame */
    bool swap_xy, mirror_x, mirror_y;   /* controller frame to the panel's native frame */
    int reset_pin;              /* -1: not connected */
    bool reset_active_high;     /* the reset level as the pin sees it (an inverting driver) */
    int interrupt_pin;          /* -1: not connected; the part polls either way */
} xiaotai_touch_i2c_config_t;

/* Jitter filter on the first touch point, in the panel's native frame. A touch
 * later than press_gap_ms after the previous sample is a new press and is
 * taken as read. Otherwise a move of at most hold_px keeps the previous point,
 * a move of at least follow_px is taken as read, and anything between moves
 * halfway. All zero: no filter. */
typedef struct {
    uint16_t press_gap_ms;
    uint8_t hold_px;
    uint8_t follow_px;
} xiaotai_touch_filter_t;

typedef struct {
    bool pressed;
    uint16_t x, y;
    int64_t sampled_ms;
} xiaotai_touch_filter_state_t;

/* [engine] One sample through the filter; zero-initialize the state. A NULL
 * point releases the press. Exposed for drivers and for host tests. */
void xiaotai_touch_filter_step(const xiaotai_touch_filter_t *filter, xiaotai_touch_filter_state_t *state,
                               int64_t now_ms, xiaotai_touch_point_t *point);

/* ------------------------------------------------------------ display part */

#define XIAOTAI_DISPLAY_POWER_STEPS_MAX 6U

/* One step of the power-up sequence: a line driven to a state, then a wait.
 * Rollback and nothing else drives the lines back, in reverse order. */
typedef struct {
    xiaotai_line_t line;
    bool active;
    uint16_t settle_ms;
} xiaotai_display_power_step_t;

typedef struct {
    /* Complete except .touch, which the part sets from the touch driver. */
    xiaotai_display_desc_t desc;
    xiaotai_panel_t panel;
    xiaotai_touch_t touch;      /* zeroed: no touch */
    /* zeroed: no backlight; set_backlight() then switches panel.set_on when
     * the panel has it, and is absent otherwise. */
    xiaotai_level_t backlight;
    xiaotai_display_power_step_t power[XIAOTAI_DISPLAY_POWER_STEPS_MAX];
    size_t power_steps;
    /* The touch controller starts before the panel: it owns a reset shared
     * with the panel and must not be reset once the panel is initialized. */
    bool touch_first;
    xiaotai_touch_filter_t touch_filter;
    /* A power key or boot table may leave the light on before open(). */
    bool backlight_off_before_start;
    /* A power step already starts the backlight at zero and stops it on rollback. */
    bool backlight_managed_in_power_steps;
    /* Stop at the first rollback error if later resources must remain powered
     * until their clients are gone. Default: attempt all remaining steps. */
    bool stop_on_rollback_error;
    /* Latch after a child start reports retained resources. Independent of
     * whether unrelated resources can still be released during rollback. */
    bool latch_on_start_cleanup_error;
    /* Nonempty: release only these power-step indexes, in the supplied order.
     * Otherwise release all reached steps in reverse order. */
    uint8_t power_rollback[XIAOTAI_DISPLAY_POWER_STEPS_MAX];
    size_t power_rollback_steps;
    /* Offer open_bound() using the same board-owned rollback order. */
    bool bind_before_commit;
} xiaotai_display_config_t;

/* [board] Creates the display part; touches no hardware. The object is
 * allocated once (XIAOTAI_MEM_INTERNAL) and never freed; the configuration is
 * copied. XIAOTAI_ERR_INVALID_ARG for a panel without start/stop/present, or a
 * descriptor that contradicts itself (SCANOUT without buffers, MONO1_VLSB
 * that is not FULL). */
xiaotai_err_t xiaotai_display_create(const xiaotai_display_config_t *config, const xiaotai_display_t **display);

/* Counters of one display part, for xiaotai_hal_report() and diagnostics. */
typedef struct {
    uint32_t opens, open_failures;
    uint32_t present_errors;
    uint32_t touch_errors;      /* failed touch reads */
    bool open;
    bool latched;               /* a rollback failed: the part refuses to open again */
    xiaotai_err_t last_error;   /* preserves the original failure when latched */
} xiaotai_display_part_stats_t;

/* [any] Never blocks. */
void xiaotai_display_part_stats(const xiaotai_display_t *display, xiaotai_display_part_stats_t *stats);

#ifdef __cplusplus
}
#endif
