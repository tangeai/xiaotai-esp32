// SPDX-License-Identifier: MIT
#include "mqtt_config.h"
#include "xiaotai/port/mem.h"
#include <string.h>

static bool field_length(const char *text, size_t *bytes)
{
    if (!text) {
        *bytes = 0;
        return true;
    }
    const char *end = text;
    while ((size_t)(end - text) <= UINT16_MAX && *end) ++end;
    const size_t len = (size_t)(end - text);
    if (len > UINT16_MAX) return false;
    *bytes = len + 1;
    return true;
}

static bool authority_valid(const char *authority)
{
    const char *p = authority;
    if (*p == '[') { /* IPv6 literal, not an unbracketed address/port pair. */
        const char *end = strchr(++p, ']');
        if (!end || end == p || !memchr(p, ':', (size_t)(end - p))) return false;
        while (p != end) {
            const unsigned char c = (unsigned char)*p++;
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') ||
                  (c >= 'a' && c <= 'f') || c == ':' || c == '.')) return false;
        }
        ++p;
    } else {
        while (*p && *p != ':') {
            const unsigned char c = (unsigned char)*p++;
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') || c == '.' || c == '-' || c == '_')) return false;
        }
        if (p == authority) return false;
    }
    if (!*p) return true;
    if (*p++ != ':' || !*p) return false;
    unsigned port = 0;
    for (; *p; ++p) {
        if (*p < '0' || *p > '9') return false;
        port = port * 10U + (unsigned)(*p - '0');
        if (port > UINT16_MAX) return false;
    }
    return port != 0;
}

xiaotai_err_t mqtt_port_config_copy(const xiaotai_mqtt_config_t *config, bool has_tls,
                                     mqtt_port_config_t *copy)
{
    if (!copy) return XIAOTAI_ERR_INVALID_ARG;
    memset(copy, 0, sizeof(*copy));
    if (!config || !config->uri || !config->client_id || !*config->client_id || !config->handler)
        return XIAOTAI_ERR_INVALID_ARG;

    const char *fields[] = {config->uri, config->client_id, config->username, config->password};
    size_t sizes[sizeof(fields) / sizeof(fields[0])];
    size_t total = 0;
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        if (!field_length(fields[i], &sizes[i])) return XIAOTAI_ERR_INVALID_ARG;
        total += sizes[i];
    }
    bool tls = strncmp(config->uri, "mqtts://", 8) == 0;
    if (!tls && strncmp(config->uri, "mqtt://", 7) != 0) return XIAOTAI_ERR_INVALID_ARG;
    if (!authority_valid(config->uri + (tls ? 8 : 7))) return XIAOTAI_ERR_INVALID_ARG;
    if (tls && !has_tls) return XIAOTAI_ERR_NOT_SUPPORTED;

    char *strings = xiaotai_mem_alloc(XIAOTAI_MEM_INTERNAL, total);
    if (!strings) return XIAOTAI_ERR_NO_MEM;
    mqtt_port_config_t out = {.value = *config, .strings = strings, .strings_bytes = total, .tls = tls};
    const char **dest[] = {&out.value.uri, &out.value.client_id, &out.value.username, &out.value.password};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i) {
        *dest[i] = sizes[i] ? strings : NULL;
        if (sizes[i]) memcpy(strings, fields[i], sizes[i]);
        strings += sizes[i];
    }
    if (!out.value.keepalive_s) out.value.keepalive_s = 60;
    if (!out.value.reconnect_ms) out.value.reconnect_ms = 3000;
    if (!out.value.rx_buffer_bytes) out.value.rx_buffer_bytes = 1024;
    if (!out.value.tx_buffer_bytes) out.value.tx_buffer_bytes = 1024;
    /* Zero keeps the library's legacy default: available memory is the limit.
     * The small-memory product profiles ask explicitly for a byte ceiling. */
    if (!out.value.task_stack_bytes) out.value.task_stack_bytes = 6144;
    *copy = out;
    return XIAOTAI_OK;
}

void mqtt_port_config_clear(mqtt_port_config_t *copy)
{
    if (!copy) return;
    volatile unsigned char *bytes = (volatile unsigned char *)copy->strings;
    for (size_t i = 0; i < copy->strings_bytes; ++i) bytes[i] = 0;
    xiaotai_mem_free(copy->strings);
    memset(copy, 0, sizeof(*copy));
}

bool mqtt_port_topics_valid(const xiaotai_mqtt_topic_t *topics, size_t count)
{
    if (!topics || !count || count > XIAOTAI_MQTT_SUBSCRIBE_MAX) return false;
    for (size_t i = 0; i < count; ++i) {
        size_t bytes;
        if (topics[i].qos > 2 || !field_length(topics[i].topic, &bytes) || bytes <= 1) return false;
    }
    return true;
}

bool mqtt_port_publish_size(const char *topic, const void *data, size_t size,
                            uint8_t qos, xiaotai_mqtt_send_t send, size_t *packet_bytes)
{
    size_t topic_bytes;
    if (!packet_bytes || qos > 1 || (!data && size) ||
        (send != XIAOTAI_MQTT_SEND_NOW && send != XIAOTAI_MQTT_SEND_QUEUED) ||
        !field_length(topic, &topic_bytes) || topic_bytes <= 1 || strpbrk(topic, "#+")) return false;
    const size_t overhead = topic_bytes + 1 + (qos ? 2 : 0);
    if (size > MQTT_MAX_REMAINING_LENGTH - overhead) return false;
    size_t remaining = overhead + size;
    size_t bytes = 1 + remaining;
    do {
        ++bytes;
        remaining /= 128;
    } while (remaining);
    *packet_bytes = bytes;
    return true;
}
