// SPDX-License-Identifier: MIT
#pragma once
#include "xiaotai/port/console.h"

#define CONSOLE_RX_DEFAULT 512U
#define CONSOLE_TX_CHUNK 224U

xiaotai_err_t console_io_open(size_t rx);
size_t console_io_read(char *data, size_t size, uint32_t timeout_ms);
xiaotai_err_t console_io_flush(void);
/* accepted includes only bytes taken by the driver; returns after drain or timeout. */
xiaotai_err_t console_io_write(const char *data, size_t size, size_t *accepted);
void console_io_lock(void);
void console_io_unlock(void);
bool console_io_connected(void);
void console_note_rx_overflow(size_t count);
