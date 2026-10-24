// SPDX-License-Identifier: MIT
#pragma once

// Capacities include the NUL terminator. Targets and credentials stay opaque.
// Parsers borrow input; xmini_rtc_connect copies the actual lengths it accepts.
#define XMINI_RTC_PEER_CAPACITY 2048U
#define XMINI_RTC_TOKEN_CAPACITY 1024U
