// SPDX-License-Identifier: MIT
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Select before open, while no reader or writer is running. */
void xiaotai_console_host_pipe(void);
size_t xiaotai_console_host_feed(const char *bytes, size_t size);
size_t xiaotai_console_host_take(char *data, size_t capacity);
void xiaotai_console_host_connected(bool connected);
/* Limit accepted wire bytes until restored with SIZE_MAX. */
void xiaotai_console_host_write_budget(size_t wire_bytes);

#ifdef __cplusplus
}
#endif
