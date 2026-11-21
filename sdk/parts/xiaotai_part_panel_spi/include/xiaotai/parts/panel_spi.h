// SPDX-License-Identifier: MIT
#pragma once
/* Parts library: colour panels on a 4-wire SPI bus, as panel drivers for the
 * display part (xiaotai/parts/display.h).
 *
 * The bus plumbing (SPI host, panel IO, chip select, reset, orientation,
 * rollback) is here once; what differs between controllers is a controller
 * object. This component carries the ST7789 controller, which the platform
 * provides by itself; controllers that need a library of their own are
 * separate components that publish one more controller object, so a board
 * pulls in only the library of the controller it carries.
 *
 * Calling tasks: create is [board]; the driver functions are called by the
 * display part only. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/parts/display.h"

#ifdef __cplusplus
extern "C" {
#endif

struct xiaotai_panel_spi_config;

/* One controller family. create() is platform code: on ESP-IDF panel_io is an
 * esp_lcd_panel_io_handle_t, device_config a const esp_lcd_panel_dev_config_t *
 * already filled from the panel configuration (vendor_config NULL unless the
 * controller sets it), and *panel receives an esp_lcd_panel_handle_t. The
 * object has static storage. */
typedef struct xiaotai_panel_spi_controller {
    const char *name;           /* "st7789" */
    xiaotai_err_t (*create)(void *panel_io, const void *device_config,
                            const struct xiaotai_panel_spi_config *config, void **panel);
    /* Optional: encode a board's init sequence into platform vendor storage.
     * The panel driver owns that allocation (xiaotai_mem_free) until its
     * native controller has been deleted. */
    xiaotai_err_t (*prepare_vendor)(const struct xiaotai_panel_spi_config *config, void **storage);
} xiaotai_panel_spi_controller_t;

/* One command of a vendor initialization sequence. */
typedef struct {
    uint8_t command;
    const uint8_t *data;        /* static storage */
    uint8_t size;
    uint16_t delay_ms;          /* wait after the command */
} xiaotai_panel_command_t;

typedef struct xiaotai_panel_spi_config {
    int host;                   /* SPI controller index of the chip */
    int mosi_pin, sclk_pin, dc_pin;
    int cs_pin;                 /* -1: chip select is cs_line */
    /* Chip select on a line (an expander pin): asserted by start() for as long
     * as the panel is started, because nothing else is on this bus. */
    xiaotai_line_t cs_line;
    xiaotai_line_t reset;       /* zeroed: the controller's software reset only; active: in reset */
    uint16_t reset_hold_ms;     /* time in reset */
    uint16_t reset_settle_ms;   /* wait after releasing reset */
    uint32_t hz;
    uint8_t spi_mode;           /* 0..3 */
    uint8_t queue_depth;        /* queued transfers */
    /* Largest single transfer; 0: one whole frame. present() splits a larger
     * area into transfers of this size itself. */
    uint32_t max_transfer_bytes;
    uint16_t native_width, native_height;   /* the controller's frame before swap_xy */
    uint16_t gap_x, gap_y;
    bool bgr;                   /* colour element order on the glass */
    bool invert;                /* colour inversion */
    bool swap_xy, mirror_x, mirror_y;   /* programmed into the controller; reported in the handles */
    /* Transfers read the caller's buffer in external RAM by DMA, with no
     * bounce buffer; the display descriptor then carries DMA_BUFFER and the
     * alignment the platform needs. */
    bool external_dma;
    /* Direct submission with an empty-command fence before returning.
     * Otherwise the driver queues from a lazily allocated, owned DMA chunk
     * and fences before reusing it. Caller bytes are free on return either
     * way. external_dma requires wait_done. */
    bool wait_done;
    /* Replaces the controller's default initialization; NULL: default. */
    const xiaotai_panel_command_t *init;
    size_t init_count;
    /* Follow the external reset pulse with a software reset when the
     * controller's bring-up sequence requires both. */
    bool software_reset_after_line;
    /* Pulse the dedicated reset line at create() rather than at start().
     * The controller's software reset, if selected, still runs at start(). */
    bool reset_line_at_create;
} xiaotai_panel_spi_config_t;

/* The ST7789 controller. */
const xiaotai_panel_spi_controller_t *xiaotai_panel_st7789(void);

/* [board] Alignment for a display descriptor whose panel reads external
 * buffers directly. Returns NOT_SUPPORTED when native direct DMA is absent. */
xiaotai_err_t xiaotai_panel_spi_external_dma_alignment(uint8_t *alignment);

/* [board] A panel driver for controller and configuration (copied; the init
 * sequence must have static storage). Touches only the reset line when
 * reset_line_at_create is selected. The driver object lives for the boot. */
xiaotai_err_t xiaotai_panel_spi_create(const xiaotai_panel_spi_controller_t *controller,
                                       const xiaotai_panel_spi_config_t *config, xiaotai_panel_t *panel);

#ifdef __cplusplus
}
#endif
