#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "tiRTC.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uart_port_t uart_port;
    gpio_num_t tx_gpio;
    gpio_num_t rx_gpio;
    gpio_num_t reset_gpio;
    uint32_t baud_rate;
    uint8_t speaker_volume;
} product_audio_config_t;

typedef struct {
    bool initialized;
    bool session_active;
    bool uplink_subscribed;
    uint32_t tx_packets;
    uint32_t tx_bytes;
    uint32_t tx_dropped;
    uint32_t tx_failed;
    uint32_t rx_packets;
    uint32_t rx_dropped;
    uint32_t rx_decode_failed;
    uint32_t pcm_blocks_played;
    uint32_t pcm_blocks_dropped;
    uint32_t playout_resyncs;
} product_audio_status_t;

typedef void (*product_voice_command_callback_t)(const char *command,
                                                  void *user_data);

typedef enum {
    PRODUCT_AUDIO_CUE_TAP,
    PRODUCT_AUDIO_CUE_CALL_ENDED,
    PRODUCT_AUDIO_CUE_WARNING,
    PRODUCT_AUDIO_CUE_WIFI_PROVISIONING,
} product_audio_cue_t;

typedef enum {
    PRODUCT_AUDIO_RING_NONE,
    PRODUCT_AUDIO_RING_INCOMING,
    PRODUCT_AUDIO_RING_OUTGOING,
} product_audio_ring_t;

esp_err_t product_audio_init(const product_audio_config_t *config);
void product_audio_set_voice_command_callback(
    product_voice_command_callback_t callback, void *user_data);

void product_audio_session_begin(tirtc_conn_t connection);
void product_audio_session_end(void);
void product_audio_set_subscribed(uint8_t stream, bool enabled);
void product_audio_tick(void);
void product_audio_receive(tirtc_conn_t connection,
                           const TIRTCFRAMEINFO *frame, const void *data);
product_audio_status_t product_audio_status(void);
void product_audio_log_status(void);

/* Local prompts are 16 kHz mono Opus stored in flash. The PCM8 streaming API
 * remains a fallback for the platform TTS endpoint. */
uint32_t product_audio_prompt_begin(void);
esp_err_t product_audio_prompt_play_code(uint32_t prompt_id,
                                         const char *six_digit_code);
esp_err_t product_audio_prompt_write_pcm8(uint32_t prompt_id,
                                          const int16_t *samples,
                                          size_t sample_count);
esp_err_t product_audio_prompt_end(uint32_t prompt_id,
                                   uint32_t timeout_ms);
void product_audio_prompt_cancel(uint32_t prompt_id);
esp_err_t product_audio_play_confirmation(void);
/* Queues a short nonblocking UI cue. Active call media always has priority. */
esp_err_t product_audio_play_cue(product_audio_cue_t cue);
/* Starts or stops the call-state ringtone. Repeating and cancellation are
 * owned by the audio component so the application never sleeps for sound. */
esp_err_t product_audio_set_ring(product_audio_ring_t ring);

#ifdef __cplusplus
}
#endif
