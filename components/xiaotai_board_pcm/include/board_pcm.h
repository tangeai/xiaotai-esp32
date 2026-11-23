#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XIAOTAI_PCM_UNUSED = 0,
    XIAOTAI_PCM_PRIMARY_MIC,
    XIAOTAI_PCM_SECONDARY_MIC,
    XIAOTAI_PCM_PLAYBACK_REFERENCE,
    XIAOTAI_PCM_LEFT,
    XIAOTAI_PCM_RIGHT,
} xiaotai_pcm_channel_t;

typedef struct {
    uint32_t sample_rate_hz;
    uint8_t channels;
    xiaotai_pcm_channel_t layout[4];
} xiaotai_pcm_format_t;

typedef struct xiaotai_pcm_port {
    xiaotai_pcm_format_t capture;
    xiaotai_pcm_format_t playback;

    // Single startup owner. Prepare does not start capture; open starts duplex I/O.
    esp_err_t (*prepare)(void);
    esp_err_t (*open)(void);

    // Blocking, interleaved signed 16-bit PCM. frames counts one sample per channel.
    // Callers own buffers; the port does not retain them. Write may modify its buffer.
    // On error transfer progress is unspecified; discard the block rather than replay it.
    esp_err_t (*read_frames)(int16_t *pcm, size_t frames);
    esp_err_t (*write_frames)(int16_t *pcm, size_t frames);

    // Serialize playback and its controls. Enable returns after hardware is ready.
    // Disabling output does not close capture or reset the shared duplex clock.
    esp_err_t (*set_output_enabled)(bool enabled);
    esp_err_t (*set_volume_percent)(uint8_t percent);
    esp_err_t (*set_muted)(bool muted);

    // Optional finite-playback drain contract. After the last payload, writing
    // this many silence frames through write_frames advances all prior payload
    // past TX DMA, including one DMA period of margin. Zero/NULL is unsupported.
    // This does not stop capture, change clocks, or prove acoustic output.
    size_t (*playback_flush_frames)(void);
} xiaotai_pcm_port_t;

// Exactly one board provider is linked per PCM product. The port is immutable.
// This interface does not represent encoded UART audio.
const xiaotai_pcm_port_t *xiaotai_board_pcm(void);

#ifdef __cplusplus
}
#endif
