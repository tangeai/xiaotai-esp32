// SPDX-License-Identifier: MIT
#pragma once

#include "xiaotai/media.h"
#include "xiaotai/media_config.h"

#if XIAOTAI_MEDIA_ALAW_FIXED_FRAMES
/* Private default receive route. Transport captures this cookie when it pins
 * its producer; loss/stop retires the cookie without changing ring contents. */
uint32_t xiaotai_media_ingress_epoch(uint32_t generation);
void xiaotai_media_ingress_break(uint32_t generation);
/* Borrowed PCMA SDK chunk: data must cover size bytes (up to UINT32_MAX).
 * False also covers backpressure after a prefix of complete blocks was kept. */
bool xiaotai_media_ingress_selected(uint32_t generation, const xiaotai_media_packet_t *packet, uint32_t epoch);
#endif
