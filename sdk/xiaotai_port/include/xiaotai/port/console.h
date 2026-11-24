// SPDX-License-Identifier: MIT
#pragma once
/* Console port: the device's primary serial console as a byte stream.
 *
 * Which device that is (a UART, a USB serial bridge, the host's standard
 * streams) is the build's choice; SDK code never names it. The port installs
 * the driver, normalizes line endings and serializes writers. Assembling
 * lines, limiting their length and parsing commands is done above the port
 * (the console engine).
 *
 * Input: one reader. Bytes arrive as received except that CR, LF and CR LF
 * each arrive as a single '\n'.
 * Output: any task. Each write, and each record, reaches the wire in one
 * piece: it is not interleaved with other writers or with log output. '\n' is
 * sent as the line ending the device uses (CR LF). A write waits until the
 * device has taken every byte, but never longer than
 * XIAOTAI_CONSOLE_WRITE_TIMEOUT_MS without progress (a USB console nobody
 * listens to): the rest of that write is then dropped and counted.
 *
 * Nothing here may be called from an interrupt handler or a real-time task. */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "xiaotai/port/err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Longest a write waits for the device without a byte being taken. */
#define XIAOTAI_CONSOLE_WRITE_TIMEOUT_MS 500U

typedef struct {
    /* Driver receive buffer; 0: the port's default (512). Input that arrives
     * while it is full is lost and counted. */
    uint16_t rx_buffer_bytes;
} xiaotai_console_config_t;

/* [init, or the reader task before its first read] Claims the console device.
 * config may be NULL. Idempotent: a second call returns XIAOTAI_OK and changes
 * nothing. XIAOTAI_ERR_NOT_SUPPORTED: this build has no console device (output
 * calls then discard and reads return 0). */
xiaotai_err_t xiaotai_console_open(const xiaotai_console_config_t *config);

/* [the one reader task] Waits up to timeout_ms for input and returns the bytes
 * copied into data, 0 on timeout. timeout_ms 0 polls. Returns as soon as at
 * least one byte is available. */
size_t xiaotai_console_read(char *data, size_t capacity, uint32_t timeout_ms);

/* [any task that may block] Writes size bytes as one uninterrupted piece.
 * XIAOTAI_OK: the device took all of them. XIAOTAI_ERR_TIMEOUT: it stopped
 * taking bytes; the rest was dropped. XIAOTAI_FAIL: no listener is attached
 * (nothing was written) or the device reported an error.
 * XIAOTAI_ERR_NOT_SUPPORTED: no console device, or not opened. */
xiaotai_err_t xiaotai_console_write(const char *data, size_t size);

/* [any task that may block] A record is several writes that must stay together
 * (a multi-line reply). begin takes the output lock, end releases it; writes of
 * the same task in between belong to the record. Records do not nest. Keep
 * them short in time: other writers and the log wait. */
void xiaotai_console_record_begin(void);
void xiaotai_console_record_end(void);

/* [the writing task] Brackets a long output (a screen dump, a recording): from
 * true to false the port keeps the calling task from starving work of lower
 * priority while it feeds the device, by whatever the platform offers (on an
 * RTOS the caller runs at the lowest priority and gets its own back). Calls do
 * not nest; every true is followed by a false on the same task. */
void xiaotai_console_bulk(bool active);

typedef struct {
    /* What the console is, for the ready line and diagnostics: for example
     * "uart0", "usb-serial", "stdio"; "" without a console. Static string. */
    const char *device;
    uint32_t baud_rate;      /* 0 when the device has none */
    bool present;
} xiaotai_console_info_t;

/* [any] */
void xiaotai_console_info(xiaotai_console_info_t *info);

typedef struct {
    uint32_t rx_bytes;
    uint32_t rx_overflows;   /* input lost because the receive buffer was full */
    uint32_t tx_bytes;       /* bytes the device took */
    uint32_t tx_timeouts;    /* writes cut short by XIAOTAI_CONSOLE_WRITE_TIMEOUT_MS */
    uint32_t tx_dropped;     /* bytes dropped by those and by writes without a listener */
    /* A listener is attached, where the device can tell (a USB console);
     * true where it cannot. */
    bool connected;
} xiaotai_console_stats_t;

/* [any] */
void xiaotai_console_stats(xiaotai_console_stats_t *stats);

#ifdef __cplusplus
}
#endif
