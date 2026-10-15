#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

typedef struct {
    char device_id[65];
    char device_key[257];
} platform_identity_t;
typedef enum {
    PLATFORM_ONLINE,
    PLATFORM_OFFLINE,
    PLATFORM_UNBOUND,
    PLATFORM_MESSAGE,
    PLATFORM_RESPONSE
} platform_event_kind_t;
typedef enum { PLATFORM_PROFILE, PLATFORM_CONTACTS, PLATFORM_CALL } platform_operation_t;
typedef struct {
    platform_event_kind_t kind;
    platform_operation_t operation;
    uint32_t generation;
    esp_err_t error;
    const char *json;
} platform_event_t;
/* Called by platform/MQTT tasks. Copy borrowed data; never block. False means
 * not accepted and prevents a command ACK so the sender can retry. */
typedef bool (*platform_observer_t)(const platform_event_t *event);

/* Prefer play_code() for local recorded prompts. The streaming callbacks are
 * retained as a bounded fallback for the signed 8 kHz platform TTS response. */
typedef struct {
    esp_err_t (*play_code)(const char *code, void *user_data);
    esp_err_t (*begin)(void *user_data);
    esp_err_t (*write_pcm8)(const int16_t *samples, size_t sample_count,
                            void *user_data);
    esp_err_t (*finish)(void *user_data);
    void (*cancel)(void *user_data);
    void *user_data;
} platform_binding_prompt_t;

/* Register before platform_start(). Passing NULL disables prompt playback. */
esp_err_t platform_set_binding_prompt(const platform_binding_prompt_t *prompt);
esp_err_t platform_start(const char *ssid, const char *password, const char *discovery_url,
                         platform_observer_t observer);
esp_err_t platform_save_wifi(const char *ssid, const char *password);
bool platform_load_wifi(char *ssid, size_t ssid_size, char *password, size_t password_size);
/* Clears the saved Wi-Fi credentials and persists a one-shot request to enter
 * provisioning after restart. A successful platform_save_wifi() clears it. */
esp_err_t platform_request_wifi_provisioning(void);
bool platform_wifi_provisioning_requested(void);
/* Clears only the local device identity. Saved Wi-Fi is intentionally kept. */
esp_err_t platform_identity_clear_local(void);
void platform_identity_snapshot(platform_identity_t *identity);
const char *platform_client_id(void);
const char *platform_rtc_endpoint(void);
bool platform_is_online(void);
/* Once WHIP is connected, WeChat answer/hangup signaling is carried by TiRTC
 * commands. Pause the MQTT/TLS transport during that interval to return its
 * internal-RAM working set to realtime media. The platform task owns the
 * stop/restart operation; callers only request the mode transition. */
void platform_set_realtime_media(bool active);
esp_err_t platform_request(platform_operation_t operation, uint32_t generation, const char *json);
/* Explicit retry after a bounded startup/provision failure, not a factory reset. */
void platform_retry(void);
