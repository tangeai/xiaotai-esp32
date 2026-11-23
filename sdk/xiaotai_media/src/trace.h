// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "xiaotai/media_config.h"
#include "xiaotai/media_format.h"
#include "xiaotai/port/err.h"

#if XIAOTAI_MEDIA_TRACE
xiaotai_err_t media_trace_register(void);
void media_trace_reset(void);
bool media_trace_active(void);
bool media_trace_rx_ingress(void);
uint8_t media_trace_db(const int16_t *pcm, size_t samples);
void media_trace_frame(bool uplink, uint32_t local_ms, const uint8_t *payload, size_t size,
                       uint8_t level, uint8_t pre, uint8_t flags, uint32_t sender_ts_ms, uint16_t duration_ms);
#else
static inline bool media_trace_active(void) { return false; }
static inline bool media_trace_rx_ingress(void) { return false; }
static inline uint8_t media_trace_db(const int16_t *pcm, size_t samples)
{
    (void)pcm; (void)samples;
    return 0;
}
static inline void media_trace_frame(bool uplink, uint32_t local_ms, const uint8_t *payload, size_t size,
                                    uint8_t level, uint8_t pre, uint8_t flags, uint32_t sender_ts_ms,
                                    uint16_t duration_ms)
{
    (void)uplink; (void)local_ms; (void)payload; (void)size; (void)level; (void)pre;
    (void)flags; (void)sender_ts_ms; (void)duration_ms;
}
#endif

#if XIAOTAI_MEDIA_TRACE && XIAOTAI_MEDIA_TRACE_ARRIVALS
void media_trace_session_open(uint32_t generation, uint32_t flags);
void media_trace_session_close(uint32_t generation);
void media_trace_arrival(uint32_t generation, uint32_t local_ms, const xiaotai_media_packet_t *packet);
#else
static inline void media_trace_session_open(uint32_t generation, uint32_t flags)
{
    (void)generation; (void)flags;
}
static inline void media_trace_session_close(uint32_t generation) { (void)generation; }
static inline void media_trace_arrival(uint32_t generation, uint32_t local_ms, const xiaotai_media_packet_t *packet)
{
    (void)generation; (void)local_ms; (void)packet;
}
#endif
