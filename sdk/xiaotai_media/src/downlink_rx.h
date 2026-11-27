// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/media.h"

void media_rx_init(void);
void media_rx_open(xiaotai_downlink_t *ring, const xiaotai_media_session_t *session, bool armed, bool muted);
void media_rx_activate(void);
void media_rx_drain(void);
/* Packets received since the session opened. */
uint32_t media_rx_received(void);
void media_rx_flush(bool accepting, bool muted);
void media_rx_close(void);
void media_rx_stats(xiaotai_media_stats_t *stats);
unsigned media_rx_writers(void);
int64_t media_rx_last_us(void);
