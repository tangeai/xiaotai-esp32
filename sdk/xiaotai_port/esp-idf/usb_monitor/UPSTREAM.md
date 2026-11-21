# USB Serial/JTAG monitor provenance

`usb_connection_monitor.c` derives from ESP-IDF's
`components/esp_driver_usb_serial_jtag/src/usb_serial_jtag_connection_monitor.c`
Copyright (c) 2023-2024 Espressif Systems (Shanghai) CO LTD. The original
copyright and SPDX notice are retained. See the [Apache-2.0 license](../../../../docs/licenses/Apache-2.0.txt).

The Xmini changes retain SOF-based connection recovery and expose read-only
diagnostics. The port preserves recovery at different FreeRTOS tick rates
and guards the replacement with an explicit opt-in and a hash allowlist
in `usb_monitor.cmake`. This is a replacement for the matching IDF source,
not a second monitor to link alongside it.
