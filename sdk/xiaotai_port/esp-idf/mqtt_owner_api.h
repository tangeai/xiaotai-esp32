// SPDX-License-Identifier: MIT
#pragma once
/* Same-component native adapter API. No private ESP-MQTT layout is exposed.
 * attach owns one internal-RAM bridge until successful owner_destroy; handler
 * and ctx are borrowed until then. Only the management caller may start/stop/
 * destroy; it serializes those calls. None is allowed from the handler.
 * start confirms task entry, not DNS, TLS or CONNACK. stop fences all callbacks
 * and closes the native connection, discarding the native outbox. */
#include "mqtt_client.h"
#include <stdbool.h>

esp_err_t xiaotai_idf_mqtt_owner_attach(esp_mqtt_client_handle_t client,
                                       esp_event_handler_t handler, void *ctx);
esp_err_t xiaotai_idf_mqtt_owner_start(esp_mqtt_client_handle_t client);
esp_err_t xiaotai_idf_mqtt_owner_stop(esp_mqtt_client_handle_t client);
esp_err_t xiaotai_idf_mqtt_owner_destroy(esp_mqtt_client_handle_t client);
bool xiaotai_idf_mqtt_owner_current(esp_mqtt_client_handle_t client);
/* [any task] Coalesced mailbox; never consumes the native event-loop slot. */
bool xiaotai_idf_mqtt_owner_wake(esp_mqtt_client_handle_t client);
