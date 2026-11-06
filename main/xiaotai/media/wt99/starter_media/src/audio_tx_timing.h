#pragma once

#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t generation, source_ms, queued_ms, begin_us;
    uint32_t gain_us, pre_encode_us, encode_us, pre_send_us, send_us, total_us;
    uint32_t pending, core, kind;
    uint32_t gain_ticks, encode_ticks, send_ticks, total_ticks;
    uint32_t encode_spi_ticks, encode_tcpip_ticks;
} audio_tx_timing_record_t;

esp_err_t audio_tx_timing_start(void);
esp_err_t audio_tx_timing_stop(void);
esp_err_t audio_tx_timing_dump(void);
#if CONFIG_APP_AUDIO_TX_TIMING_DIAGNOSTICS
esp_err_t audio_tx_timing_worker_started(void);
uint32_t audio_tx_timing_ticks(void);
void audio_tx_timing_network_ticks(unsigned core, uint32_t *spi, uint32_t *tcpip);
void audio_tx_timing_record(const audio_tx_timing_record_t *record);
void audio_tx_timing_drop(uint32_t generation, uint32_t source_ms);
#else
static inline void audio_tx_timing_record(const audio_tx_timing_record_t *record) { (void)record; }
static inline void audio_tx_timing_drop(uint32_t generation, uint32_t source_ms)
{ (void)generation; (void)source_ms; }
#endif
