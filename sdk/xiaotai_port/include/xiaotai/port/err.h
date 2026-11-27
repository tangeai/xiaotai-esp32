// SPDX-License-Identifier: MIT
#pragma once
/* SDK result codes. The values equal ESP-IDF's esp_err_t codes, so the ESP-IDF
 * port passes platform results through unchanged. */

typedef int xiaotai_err_t;

#define XIAOTAI_OK 0
#define XIAOTAI_FAIL (-1)
#define XIAOTAI_ERR_NO_MEM 0x101
#define XIAOTAI_ERR_INVALID_ARG 0x102
#define XIAOTAI_ERR_INVALID_STATE 0x103
#define XIAOTAI_ERR_INVALID_SIZE 0x104
#define XIAOTAI_ERR_NOT_FOUND 0x105
#define XIAOTAI_ERR_NOT_SUPPORTED 0x106
#define XIAOTAI_ERR_TIMEOUT 0x107
#define XIAOTAI_ERR_INVALID_RESPONSE 0x108
#define XIAOTAI_ERR_INVALID_CRC 0x109 /* checksum or digest mismatch */

/* SDK-specific results, outside ESP-IDF's ranges. Every function returns the
 * codes above wherever one fits; a component adds a code only for a condition
 * its callers must tell apart, takes it from its own block of 0x100 and
 * defines it in its own header:
 *   0x7000..0x70FF  shared by every component (below)
 *   0x7100..0x71FF  platform core (xiaotai/platform.h)
 *   0x7200..0x72FF  console engine (xiaotai/at_command.h)
 * A new block is the next free one and is added to this list. */
#define XIAOTAI_ERR_BUSY 0x7001 /* the resource is held: a foreground activity, a session, a pending request */
/* Refused by policy whatever the state: a local-only intent asked for over an
 * open channel such as the console (xiaotai/framework.h). */
#define XIAOTAI_ERR_NOT_ALLOWED 0x7002
