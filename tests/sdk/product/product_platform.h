// SPDX-License-Identifier: MIT
#pragma once

#include <stdbool.h>
#include "xiaotai_http_host.h"
#include "xiaotai_mqtt_host.h"

#define PRODUCT_FIXTURE_DEVICE "host-device"
#define PRODUCT_FIXTURE_SECRET "host-device-key"
#define PRODUCT_FIXTURE_CLIENT "sn_" PRODUCT_FIXTURE_DEVICE
#define PRODUCT_FIXTURE_GUEST "host-binding"

/* Install only in-process port scripts and optionally seed this generated
 * product's deployed identity layout in the host KV store. */
void product_platform_prepare(bool bound);
void product_platform_http_responder(xiaotai_http_host_responder_fn responder, void *context);
void product_platform_login_observer(void (*observe)(const xiaotai_http_host_request_t *));
void product_platform_login_override(bool (*override)(const xiaotai_http_host_request_t *,
                                                      xiaotai_http_host_reply_t *));
void product_platform_profile_observer(void (*observe)(const xiaotai_http_host_request_t *));
/* Fixture-owned static body, set before starting the product. */
void product_platform_device_contacts_body(const char *body);
void product_platform_discovery_response(int status, const char *body, const char *expected_broker);
void product_platform_discovery_body(const char *body);
void product_platform_discovery_status(int status);
uint32_t product_platform_network_token(void);
void product_platform_link_ready(void);
void product_platform_online(void);
bool product_platform_has_client(const char *client_id);
void product_platform_withhold_puback(const char *client_id);
xiaotai_err_t product_platform_take_publication(const char *client_id, xiaotai_mqtt_host_publication_t *out,
                                               char *topic, size_t topic_capacity, void *body, size_t body_capacity);
void product_platform_message_to(const char *client_id, const char *suffix, const char *json);
void product_platform_message(const char *suffix, const char *json);
