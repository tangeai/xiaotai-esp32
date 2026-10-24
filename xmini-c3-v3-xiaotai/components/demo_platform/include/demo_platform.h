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
    PLATFORM_PHASE_IDLE, PLATFORM_PHASE_CONNECTING, PLATFORM_PHASE_TIME,
    PLATFORM_PHASE_DISCOVERING, PLATFORM_PHASE_REPORTING, PLATFORM_PHASE_BINDING,
    PLATFORM_PHASE_VERIFYING, PLATFORM_PHASE_ONLINE, PLATFORM_PHASE_OFFLINE,
    PLATFORM_PHASE_ERROR,
} platform_phase_t;

typedef struct {
    platform_phase_t phase;
    esp_err_t error;
    char code[7];
    int64_t expires_at_ms;
    unsigned revision;
} platform_status_t;

platform_status_t platform_status(void);
typedef enum {
    PLATFORM_ONLINE,
    PLATFORM_OFFLINE,
    PLATFORM_UNBOUND,
    PLATFORM_MESSAGE,
    PLATFORM_RESPONSE
} platform_event_kind_t;
#define PLATFORM_JSON_CAPACITY 8192U
typedef enum {
    PLATFORM_PROFILE, PLATFORM_CONTACTS, PLATFORM_CALL,
    PLATFORM_AI_TOKEN,
    PLATFORM_DEVICE_CONTACTS,
    PLATFORM_DEVICE_CALL_REQUEST, PLATFORM_DEVICE_CALL_INFO,
    PLATFORM_DEVICE_CALL_REJECT, PLATFORM_DEVICE_CALL_CANCEL,
    PLATFORM_DEVICE_CALL_HANGUP, PLATFORM_DEVICE_CALL_ROOM,
    PLATFORM_DEVICE_CONTACTS_PENDING, PLATFORM_DEVICE_CONTACT_REQUEST,
    PLATFORM_DEVICE_CONTACT_RESPOND, PLATFORM_DEVICE_CONTACT_REMARK,
    PLATFORM_DEVICE_CONTACT_DELETE,
    PLATFORM_GROUP_ASSIGNMENT, PLATFORM_GROUP_CREATE, PLATFORM_GROUP_JOIN,
    PLATFORM_GROUP_LEAVE, PLATFORM_GROUP_CONNECT_TOKEN, PLATFORM_GROUP_PRESENCE,
    PLATFORM_OPERATION_COUNT,
} platform_operation_t;
typedef struct {
    platform_event_kind_t kind;
    platform_operation_t operation;
    uint32_t generation;
    unsigned epoch;
    esp_err_t error;
    bool delivery_failed;
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
/* Requires a drained HTTP-exclusive ticket and root-owned RTC maintenance.
 * Persist only the provisioning-entry flag, retaining old Wi-Fi and identity.
 * Root explicitly restarts after user confirmation; verified save clears it. */
esp_err_t platform_request_wifi_provisioning(uint32_t ticket);
bool platform_wifi_provisioning_requested(void);
/* Clears only the local device identity. Saved Wi-Fi is intentionally kept. */
esp_err_t platform_identity_clear_local(void);
void platform_identity_snapshot(platform_identity_t *identity);
const char *platform_client_id(void);
const char *platform_rtc_endpoint(void);
bool platform_is_online(void);
/* ESP-IDF high-water mark is bytes; false/zero means no live worker, not overflow. */
bool platform_stack_watermark(unsigned *bytes);
unsigned platform_epoch(void);
typedef struct {
    uint32_t ticket;
    bool held, ready, in_flight;
    unsigned pending;
} platform_http_exclusive_status_t;
/* C3 keeps MQTT resident. Begin fences new HTTP admission, then the platform
 * worker drains already-admitted HTTP and any current startup operation.
 * Root polls ready before network reconfiguration and always ends its own
 * ticket on cancellation/failure. No waiting or TLS work runs on the caller. */
esp_err_t platform_http_exclusive_begin(uint32_t *ticket);
bool platform_http_exclusive_ready(uint32_t ticket);
esp_err_t platform_http_exclusive_end(uint32_t ticket);
platform_http_exclusive_status_t platform_http_exclusive_status(void);
esp_err_t platform_request(platform_operation_t operation, uint32_t generation, const char *json);
/* On successful queue admission only, returns the exact epoch owned by that
 * queued request. Failure leaves the output unchanged; NULL is permitted. */
esp_err_t platform_request_epoch(platform_operation_t operation, uint32_t generation,
                                 const char *json, unsigned *accepted_epoch);
/* PLATFORM_RESPONSE borrows the complete server JSON envelope, not data-only.
 * Copy it before returning. Bodies are JSON objects for POST/PUT/DELETE and
 * must be NULL/empty for GET. Acceptance only queues the HTTP operation. */
/* Drain on the control task before processing new business actions. These
 * terminal events mean the remote result could not be delivered, not that a
 * request with side effects was rejected by the server. */
bool platform_take_response_failure(platform_event_t *event);
/* One controller consumes at most three per tick before ordinary business
 * events. Lifecycle delivery is sticky, independent of the bounded JSON queue;
 * UNBOUND is retained and returned before any subsequent ONLINE. */
bool platform_take_lifecycle(platform_event_t *event);
/* Explicit retry after a bounded startup/provision failure, not a factory reset. */
esp_err_t platform_retry(void);
#define PLATFORM_NETWORK_HISTORY 8U
typedef struct {
    uint8_t reason;
    int8_t rssi;
    uint32_t uptime_ms;
} platform_network_disconnect_t;
/* first_/last_ describe the current outage, or the previous one while connected;
 * attempts counts disconnects since the last IP. backoff_ms is the armed delay,
 * 0 when none. history is oldest first. Uptimes match ESP_LOG timestamps. */
typedef struct {
    bool connected, stopped;
    esp_err_t error;
    unsigned attempts;
    uint32_t backoff_ms;
    uint8_t first_reason, last_reason;
    int8_t first_rssi, last_rssi;
    uint32_t first_uptime_ms;
    unsigned disconnects, beacon_timeouts, history_count;
    platform_network_disconnect_t history[PLATFORM_NETWORK_HISTORY];
} platform_network_diag_t;
/* Read-only station snapshot. ESP_ERR_INVALID_STATE, still filled, when the
 * station was never started (provisioning boot). */
esp_err_t platform_network_diag(platform_network_diag_t *diag);
