// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define XMINI_PCMA_RATE 8000
#define XMINI_PCMA_FRAME_MS 20
#define XMINI_PCMA_SAMPLES 160

typedef enum {
    XMINI_AUDIO_VOLUME = 1,
    XMINI_AUDIO_GAIN,
    XMINI_AUDIO_TONE,
    XMINI_AUDIO_RECORD,
    XMINI_AUDIO_PLAY,
    XMINI_AUDIO_STOP,
    XMINI_AUDIO_PROMPT,
    XMINI_AUDIO_STREAM,
} xmini_audio_command_t;

typedef enum {
    XMINI_PROMPT_STARTUP,
    XMINI_PROMPT_WIFI_SETUP,
    XMINI_PROMPT_WIFI_CONNECTING,
    XMINI_PROMPT_WIFI_CONNECTED,
    XMINI_PROMPT_WIFI_FAILED,
    XMINI_PROMPT_BINDING,
    XMINI_PROMPT_BOUND,
    XMINI_PROMPT_OFFLINE,
    XMINI_PROMPT_ONLINE,
    XMINI_PROMPT_INCOMING,
    XMINI_PROMPT_DIALING,
    XMINI_PROMPT_ENDED,
    XMINI_PROMPT_FAILED,
    XMINI_PROMPT_COUNT,
} xmini_prompt_t;

typedef enum { XMINI_AUDIO_DIAGNOSTIC, XMINI_AUDIO_GUIDANCE, XMINI_AUDIO_RTC } xmini_audio_owner_t;

typedef bool (*xmini_audio_tx_t)(uint32_t generation, const uint8_t *packet,
                               size_t length, uint64_t pts_ms, void *context);

typedef struct {
    xmini_audio_command_t kind;
    xmini_audio_owner_t owner;
    int value;
    char code[7];
    uint32_t generation;
    xmini_audio_tx_t transmit;
    void *context;
} xmini_audio_request_t;

typedef struct {
    bool ready;
    bool busy;
    bool device_open;
    bool recording_ready;
    bool error;
    int volume;
    int gain_db;
    unsigned frames;
    uint32_t peak;
    int prompt;
    unsigned expected_frames;
    bool stopped;
    xmini_audio_owner_t owner;
    uint32_t generation;
    uint32_t tx_frames, rx_frames, tx_dropped, rx_dropped, underruns;
    uint32_t injected_frames;
    unsigned recording_source; // 0: microphone diagnostic, 1: received PCMA
    bool echo_enabled;
    uint32_t echo_closed_frames, echo_dt_frames, echo_open_frames;
    uint32_t echo_gain_q15; // last stream uplink gain, 32768 is unity
} xmini_audio_status_t;

esp_err_t xmini_audio_init(void);
esp_err_t xmini_audio_initial_settings(int volume, int gain_db);
// Nonblocking; one worker owns codec, I2S and all recording/playback state.
// Generic STOP is for non-RTC audio; RTC cancellation requires stream_stop(generation).
esp_err_t xmini_audio_request(xmini_audio_request_t request);
xmini_audio_status_t xmini_audio_status(void);
// ESP-IDF reports bytes. False means no worker; zero with true is a real sample.
bool xmini_audio_stack_watermark(unsigned *bytes);
esp_err_t xmini_audio_prompt(xmini_prompt_t prompt, const char *code);
esp_err_t xmini_audio_stream_start(uint32_t generation, xmini_audio_tx_t transmit, void *context);
esp_err_t xmini_audio_stream_stop(uint32_t generation);
bool xmini_audio_stream_busy(uint32_t generation);
void xmini_audio_stream_flush(uint32_t generation);
esp_err_t xmini_audio_stream_tone(uint32_t generation);
// RTC uplink echo suppressor, enabled by default; a change applies from the next stream frame.
esp_err_t xmini_audio_echo_guard_set(bool enabled);
bool xmini_audio_echo_guard_enabled(void);
esp_err_t xmini_audio_recording_copy(uint8_t *out, size_t capacity);
// Callback-safe bounded copy. Late frames cannot enter a newer audio session.
bool xmini_audio_receive(uint32_t generation, const uint8_t *packet, size_t length);
