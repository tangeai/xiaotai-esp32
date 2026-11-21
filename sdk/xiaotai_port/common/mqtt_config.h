// SPDX-License-Identifier: MIT
#pragma once
#include "xiaotai/port/mqtt.h"

#define MQTT_PORT_VERSION 4U /* MQTT 3.1.1 protocol level */
#define MQTT_MAX_REMAINING_LENGTH 268435455U

typedef struct {
    xiaotai_mqtt_config_t value;
    char *strings;
    size_t strings_bytes;
    bool tls;
} mqtt_port_config_t;

xiaotai_err_t mqtt_port_config_copy(const xiaotai_mqtt_config_t *config, bool has_tls,
                                     mqtt_port_config_t *copy);
void mqtt_port_config_clear(mqtt_port_config_t *copy);

/* Embedded backends use these same validation and packet accounting rules. */
bool mqtt_port_topics_valid(const xiaotai_mqtt_topic_t *topics, size_t count);
bool mqtt_port_publish_size(const char *topic, const void *data, size_t size,
                            uint8_t qos, xiaotai_mqtt_send_t send, size_t *packet_bytes);
