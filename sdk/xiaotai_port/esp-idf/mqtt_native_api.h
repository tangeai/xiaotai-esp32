// SPDX-License-Identifier: MIT
#pragma once
#include <stddef.h>
#include "esp_err.h"
#include "mqtt_client.h"

/* Private port/native boundary; inputs have passed the port validators.
 * SUBSCRIBE distinguishes native disconnected from allocation/write FAIL.
 * Publishes take complete MQTT 3.1.1 wire bytes, QoS0/1, retain=false.
 * Failure always clears *message_id. QUEUED uses only the native outbox short
 * transaction; SUBSCRIBE and SEND_NOW may wait for network I/O. */
esp_err_t xiaotai_idf_mqtt_subscribe_connected(esp_mqtt_client_handle_t client,
                                             const esp_mqtt_topic_t *topics, int count, int *message_id);
esp_err_t xiaotai_idf_mqtt_enqueue_connected(esp_mqtt_client_handle_t client, const char *topic,
                                           const char *data, int len, int qos, size_t wire_bytes,
                                           int *message_id);
esp_err_t xiaotai_idf_mqtt_publish_now(esp_mqtt_client_handle_t client, const char *topic,
                                     const char *data, int len, int qos, size_t wire_bytes,
                                     int *message_id);
