// SPDX-License-Identifier: MIT
#include "product_platform.h"
#include "xiaotai/identity.h"
#include "xiaotai/net.h"
#include "xiaotai/platform.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/os.h"
#include "xiaotai/transport.h"
#include "xiaotai_clock_host.h"
#include "xiaotai_mqtt_host.h"
#include "xiaotai_net_host.h"
#include "xiaotai_product.h"
#include "xiaotai_transport_host.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WAIT_MS 10000U
#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); abort(); \
} } while (0)

static const int64_t wall_ms = INT64_C(1700000100000);
static const char services[] =
    "{\"device-srv\":\"http://fixture.invalid/device\",\"mqtt-srv\":\"mqtts://fixture.invalid\","
    "\"ai-srv\":\"http://fixture.invalid/ai\",\"call-srv\":\"http://fixture.invalid/call\","
    "\"voip-srv\":\"http://fixture.invalid/voip\",\"tirtc-srv\":\"http://fixture.invalid/rtc\","
    "\"recognition-srv\":\"http://fixture.invalid/recognition\"}";
/* The unverified payload expires 400s after the fixed fixture wall clock. */
static const char login[] = "{\"code\":200,\"data\":{\"mqtt_token\":\"a.eyJleHAiOjE3MDAwMDA1MDB9.b\"}}";
static const char ai_token[] = "{\"code\":200,\"data\":{\"peer_id\":\"fixture-ai-peer\","
    "\"token\":\"fixture-ai-token\",\"role_id\":\"fixture-role\"}}";
static const char okay[] = "{\"code\":200,\"data\":[]}";
static const char voip_contacts[] = "{\"code\":0,\"data\":{\"contacts\":[]}}";
static xiaotai_http_host_responder_fn custom_http;
static void *custom_context;
static void (*login_observer)(const xiaotai_http_host_request_t *);
static bool (*login_override)(const xiaotai_http_host_request_t *, xiaotai_http_host_reply_t *);
static void (*profile_observer)(const xiaotai_http_host_request_t *);
static uint16_t packet_id;
static _Atomic(const char *) discovery_override;
static const char *discovery_broker;
static atomic_int discovery_override_status;

static pthread_mutex_t broker_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    xiaotai_mqtt_t *client;
    char id[96];
} brokers[4];

xiaotai_err_t __real_xiaotai_mqtt_create(const xiaotai_mqtt_config_t *cfg, xiaotai_mqtt_t **out);
xiaotai_err_t __wrap_xiaotai_mqtt_create(const xiaotai_mqtt_config_t *cfg, xiaotai_mqtt_t **out);
xiaotai_err_t __real_xiaotai_mqtt_destroy(xiaotai_mqtt_t *client);
xiaotai_err_t __wrap_xiaotai_mqtt_destroy(xiaotai_mqtt_t *client);

xiaotai_err_t __wrap_xiaotai_mqtt_create(const xiaotai_mqtt_config_t *cfg, xiaotai_mqtt_t **out)
{
    if (discovery_broker) CHECK(!strcmp(cfg->uri, discovery_broker));
    const xiaotai_err_t err = __real_xiaotai_mqtt_create(cfg, out);
    if (err != XIAOTAI_OK) return err;
    pthread_mutex_lock(&broker_lock);
    size_t i = 0;
    while (i < sizeof(brokers) / sizeof(*brokers) && brokers[i].client) ++i;
    CHECK(i < sizeof(brokers) / sizeof(*brokers));
    CHECK(strlen(cfg->client_id) < sizeof(brokers[i].id));
    strcpy(brokers[i].id, cfg->client_id);
    brokers[i].client = *out;
    pthread_mutex_unlock(&broker_lock);
    return XIAOTAI_OK;
}

xiaotai_err_t __wrap_xiaotai_mqtt_destroy(xiaotai_mqtt_t *client)
{
    pthread_mutex_lock(&broker_lock);
    size_t index = 0;
    while (index < sizeof(brokers) / sizeof(*brokers) && brokers[index].client != client) ++index;
    const xiaotai_err_t err = __real_xiaotai_mqtt_destroy(client);
    if (err == XIAOTAI_OK && index < sizeof(brokers) / sizeof(*brokers)) brokers[index].client = NULL;
    pthread_mutex_unlock(&broker_lock);
    return err;
}

static xiaotai_mqtt_t *find_broker(const char *client_id)
{
    for (size_t i = 0; i < sizeof(brokers) / sizeof(*brokers); ++i)
        if (brokers[i].client && !strcmp(brokers[i].id, client_id)) return brokers[i].client;
    return NULL;
}

bool product_platform_has_client(const char *client_id)
{
    pthread_mutex_lock(&broker_lock);
    const bool found = find_broker(client_id) != NULL;
    pthread_mutex_unlock(&broker_lock);
    return found;
}

void product_platform_withhold_puback(const char *client_id)
{
    pthread_mutex_lock(&broker_lock);
    xiaotai_mqtt_t *client = find_broker(client_id);
    CHECK(client);
    xiaotai_mqtt_host_puback(client, false);
    pthread_mutex_unlock(&broker_lock);
}

xiaotai_err_t product_platform_take_publication(const char *client_id, xiaotai_mqtt_host_publication_t *out,
                                               char *topic, size_t topic_capacity, void *body, size_t body_capacity)
{
    pthread_mutex_lock(&broker_lock);
    xiaotai_mqtt_t *client = find_broker(client_id);
    const xiaotai_err_t err = client ?
        xiaotai_mqtt_host_take(client, out, topic, topic_capacity, body, body_capacity) : XIAOTAI_ERR_NOT_FOUND;
    pthread_mutex_unlock(&broker_lock);
    return err;
}

void product_platform_message_to(const char *client_id, const char *suffix, const char *json)
{
    char topic[128];
    CHECK(!strcmp(suffix, "cmd") || !strcmp(suffix, "notify"));
    const int length = snprintf(topic, sizeof(topic), "device/%s/%s", client_id, suffix);
    CHECK(length > 0 && (size_t)length < sizeof(topic));
    pthread_mutex_lock(&broker_lock);
    xiaotai_mqtt_t *client = find_broker(client_id);
    CHECK(client != NULL);
    if (++packet_id == 0) ++packet_id;
    const xiaotai_err_t err = xiaotai_mqtt_host_message(client, topic, json, strlen(json), 1, packet_id,
                                                       false, false, NULL, 0);
    pthread_mutex_unlock(&broker_lock);
    CHECK(err == XIAOTAI_OK);
}

void product_platform_message(const char *suffix, const char *json)
{
    product_platform_message_to(PRODUCT_FIXTURE_CLIENT, suffix, json);
}

static const char *device_contacts;

void product_platform_device_contacts_body(const char *body)
{
    device_contacts = body;
}

void product_platform_http_responder(xiaotai_http_host_responder_fn responder, void *context)
{
    /* Install before product start, never concurrently with the platform worker. */
    custom_http = responder;
    custom_context = context;
}

void product_platform_login_observer(void (*observe)(const xiaotai_http_host_request_t *))
{
    login_observer = observe;
}

void product_platform_login_override(bool (*override)(const xiaotai_http_host_request_t *,
                                                      xiaotai_http_host_reply_t *))
{
    login_override = override;
}

void product_platform_profile_observer(void (*observe)(const xiaotai_http_host_request_t *))
{
    profile_observer = observe;
}

void product_platform_discovery_response(int status, const char *body, const char *expected_broker)
{
    CHECK(status > 0 && body);
    product_platform_discovery_status(status);
    discovery_broker = expected_broker;
    product_platform_discovery_body(body);
}

void product_platform_discovery_body(const char *body) { atomic_store(&discovery_override, body); }

void product_platform_discovery_status(int status) { atomic_store(&discovery_override_status, status); }

static void response(void *context, const xiaotai_http_host_request_t *req, xiaotai_http_host_reply_t *reply)
{
    (void)context;
    const char *path = strstr(req->url, "://");
    if (!path || !(path = strchr(path + 3, '/'))) { reply->status = 404; return; }
    const char *body = NULL;
    if (!strcmp(path, "/services") && req->method == XIAOTAI_HTTP_GET) {
        const char *override = atomic_load(&discovery_override);
        if (override) {
            CHECK(!strncmp(req->url, "http://", sizeof("http://") - 1));
            reply->status = atomic_load(&discovery_override_status);
            if (reply->status == 302) {
                static const xiaotai_http_header_t location[] = {{"Location", "http://no-follow.invalid/elsewhere"}};
                reply->headers = location;
                reply->header_count = 1;
            }
            reply->body = (const uint8_t *)override;
            reply->body_size = strlen(override);
            return;
        }
        body = services;
    }
    else if (!strcmp(path, "/device/v1/device/token") && req->method == XIAOTAI_HTTP_POST) {
        if (login_observer) login_observer(req);
        if (login_override && login_override(req, reply)) return;
        body = login;
    }
    else if (!strcmp(path, "/ai/v1/ai/token")) {
        CHECK(!strcmp(req->url, "http://fixture.invalid/ai/v1/ai/token") &&
              req->method == XIAOTAI_HTTP_GET && !req->body && !req->body_size);
        unsigned auth = 0;
        for (size_t i = 0; i < req->header_count; ++i) {
            if (strcmp(req->headers[i].name, "Authorization")) continue;
            CHECK(!strcmp(req->headers[i].value, "Bearer a.eyJleHAiOjE3MDAwMDA1MDB9.b"));
            ++auth;
        }
        CHECK(auth == 1);
        body = ai_token;
    }
    else if (!strcmp(path, "/device/v1/device/profile")) {
        if (profile_observer) profile_observer(req);
        body = okay;
    }
    else if (!strcmp(path, "/call/v1/call/device/contacts")) body = device_contacts ? device_contacts : okay;
    else if (req->method == XIAOTAI_HTTP_GET &&
             strcspn(path, "?") == sizeof("/voip/v1/voip/device/contacts") - 1 &&
             !strncmp(path, "/voip/v1/voip/device/contacts", sizeof("/voip/v1/voip/device/contacts") - 1))
        body = voip_contacts;
    else if (custom_http) {
        custom_http(custom_context, req, reply);
        return;
    }
    if (!body) { reply->status = 404; return; }
    reply->status = 200;
    reply->body = (const uint8_t *)body;
    reply->body_size = strlen(body);
}

static void seed_identity(void)
{
    if (!strcmp(XIAOTAI_PRODUCT_NAME, "xiaotai-xmini-c3-v3")) {
        /* Xmini uses the deployed one-record layout. */
        struct { char id[65]; char secret[257]; } record = {PRODUCT_FIXTURE_DEVICE, PRODUCT_FIXTURE_SECRET};
        CHECK(xiaotai_kv_set("xmini", "identity", &record, sizeof(record), XIAOTAI_KV_WAIT_MAX_MS) == XIAOTAI_OK);
    } else {
        CHECK(xiaotai_kv_set_str("tirtc_cfg", "device_id", PRODUCT_FIXTURE_DEVICE, XIAOTAI_KV_WAIT_MAX_MS) == XIAOTAI_OK);
        CHECK(xiaotai_kv_set_str("tirtc_cfg", "secret", PRODUCT_FIXTURE_SECRET, XIAOTAI_KV_WAIT_MAX_MS) == XIAOTAI_OK);
    }
}

void product_platform_prepare(bool bound)
{
    if (bound) seed_identity();
    xiaotai_clock_host_set_wall_ms(wall_ms);
    xiaotai_clock_host_answer(wall_ms, 0);
    xiaotai_clock_host_probe_result(true, wall_ms, 0);
    xiaotai_transport_host_auto_started(true);
    xiaotai_transport_host_auto_stopped(true);
    xiaotai_http_host_set_responder(response, NULL, true);
}

static xiaotai_net_host_record_t network_join(void)
{
    CHECK(xiaotai_net_host_wait_calls(XIAOTAI_NET_HOST_JOIN, 1, WAIT_MS));
    static xiaotai_net_host_record_t recorded[XIAOTAI_NET_HOST_RECORDS_MAX];
    const size_t count = xiaotai_net_host_records(recorded, sizeof(recorded) / sizeof(*recorded));
    for (size_t i = count; i > 0; --i) {
        if (recorded[i - 1].call == XIAOTAI_NET_HOST_JOIN) return recorded[i - 1];
    }
    fprintf(stderr, "host JOIN call missing from the Wi-Fi fixture\n");
    abort();
}

uint32_t product_platform_network_token(void) { return network_join().token; }

void product_platform_link_ready(void)
{
    xiaotai_net_status_t network;
    xiaotai_net_status(&network);
    if (network.state == XIAOTAI_NET_UP) return;
    const xiaotai_net_host_record_t joined = network_join();
    CHECK(joined.token && joined.has_credentials);
    xiaotai_net_driver_event_t evt = {.kind = XIAOTAI_NET_DRIVER_JOINED, .token = joined.token};
    strcpy(evt.ssid, joined.credentials.ssid);
    CHECK(xiaotai_net_host_emit(&evt) == XIAOTAI_OK);
    evt = (xiaotai_net_driver_event_t){.kind = XIAOTAI_NET_DRIVER_ADDRESS, .token = joined.token,
                                     .ipv4 = UINT32_C(0x020000C0)};
    CHECK(xiaotai_net_host_emit(&evt) == XIAOTAI_OK);
    for (unsigned attempt = 0; attempt < WAIT_MS / 5; ++attempt) {
        xiaotai_net_status(&network);
        if (network.state == XIAOTAI_NET_UP) return;
        xiaotai_delay_ms(5);
    }
    fprintf(stderr, "host Wi-Fi not up: state=%u\n", (unsigned)network.state);
    abort();
}

void product_platform_online(void)
{
    product_platform_link_ready();
    xiaotai_platform_status_t status;
    xiaotai_transport_status_t rtc;
    for (unsigned attempt = 0; attempt < WAIT_MS / 5; ++attempt) {
        xiaotai_platform_status(&status);
        xiaotai_transport_status(&rtc);
        if (status.phase == XIAOTAI_PLATFORM_ONLINE && status.signaling &&
            rtc.state == XIAOTAI_TRANSPORT_ONLINE && product_platform_has_client(PRODUCT_FIXTURE_CLIENT)) return;
        xiaotai_delay_ms(5);
    }
    fprintf(stderr, "host platform not ready: phase=%u signal=%u error=%d RTC state=%u wait=%u\n",
            (unsigned)status.phase, (unsigned)status.signaling, (int)status.last_error,
            (unsigned)rtc.state, (unsigned)rtc.wait);
    abort();
}
