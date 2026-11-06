#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif
/* Local diagnostic: last 32 sampled SOF bits at a disconnect decision (LSB is
 * newest), and cumulative decisions. Read-only; does not reset the monitor. */
uint32_t usb_serial_jtag_get_last_disconnect_sof(uint32_t *count);

#define USB_SERIAL_JTAG_SOF_DIAG_SAMPLES 16
#define USB_SERIAL_JTAG_SOF_SAMPLE_OBSERVED 1U
#define USB_SERIAL_JTAG_SOF_SAMPLE_CLEARED 2U
#define USB_SERIAL_JTAG_SOF_SAMPLE_RAW_AFTER 4U

/** Local read-only timing evidence for the last disconnect decision.
 * Arrays are oldest first, newest last; sof_history bit 0 is newest.
 * frame_index contains the 11-bit hardware SOF index on C3 when valid.
 * sample_flags records raw observed, W1C executed, and raw after W1C.
 * Call from task context. No counters or connection state are cleared. */
typedef struct {
    uint64_t timestamp_us;
    uint32_t disconnect_count;
    uint32_t sof_history;
    uint32_t since_sof_us;
    uint32_t missing_ticks;
    uint32_t delta_us[USB_SERIAL_JTAG_SOF_DIAG_SAMPLES];
    uint16_t frame_index[USB_SERIAL_JTAG_SOF_DIAG_SAMPLES];
    uint8_t sample_flags[USB_SERIAL_JTAG_SOF_DIAG_SAMPLES];
    uint8_t frame_index_valid;
} usb_serial_jtag_sof_diagnostic_t;

bool usb_serial_jtag_get_sof_diagnostic(usb_serial_jtag_sof_diagnostic_t *diagnostic);

#if CONFIG_XMINI_USB_TIMING_PROBE
/** Diagnostic builds only: mask CPU IRQs for 8 ms once per boot.
 * Call from task context while the host is attached. This intentionally
 * exercises SYSTIMER tick catch-up; it does not modify USB registers. */
esp_err_t usb_serial_jtag_test_delay_tick_once(void);
#endif
#ifdef __cplusplus
}
#endif
