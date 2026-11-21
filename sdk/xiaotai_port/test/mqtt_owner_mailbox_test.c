// SPDX-License-Identifier: MIT
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "mqtt_owner_mailbox.h"

typedef int esp_err_t;
enum { ESP_FAIL = -1, ESP_OK = 0, MQTT_USER_EVENT = 99, MQTT_PROTOCOL_V_5 = 5 };
#define MQTT_SUPPORTED_FEATURE_EVENT_LOOP 1
#define MQTT_EVENTS "mqtt"
#define portMAX_DELAY UINT32_MAX

typedef struct {
    void *client;
    int protocol_ver, event_id;
} esp_mqtt_event_t;
typedef struct { void *event_loop_handle; } native_config_t;
typedef struct {
    native_config_t *config;
    struct { struct { struct { int protocol_ver; } information; } connection; } mqtt_state;
    esp_mqtt_event_t event;
} native_client_t, *esp_mqtt_client_handle_t;

static unsigned queued, blocked_posts, native_events, wake_events;
static bool owner;

static esp_err_t esp_event_post_to(void *loop, const char *base, int id,
                                    const void *data, size_t size, unsigned ticks)
{
    (void)loop; (void)base; (void)id; (void)data;
    assert(size == sizeof(esp_mqtt_event_t));
    if (queued == 1U) {
        /* Observe the native request for an unbounded wait, without hanging
         * the test process. The only queue runner has not been reached. */
        assert(ticks == portMAX_DELAY);
        ++blocked_posts;
        return ESP_FAIL;
    }
    ++queued;
    return ESP_OK;
}

static esp_err_t esp_event_loop_run(void *loop, unsigned ticks)
{
    (void)loop;
    assert(ticks == 0U && owner);
    native_events += queued;
    queued = 0;
    return ESP_OK;
}

/* An exact function from the hash-verified native 1.1.0 client. */
#include "native_dispatch.inc"

static void *request(void *ctx)
{
    xt_mqtt_owner_mailbox_t *box = ctx;
    for (unsigned i = 0; i < 1000U; ++i) assert(xt_mqtt_mailbox_request(box));
    return NULL;
}

int main(void)
{
    native_client_t native = {0};
    native_config_t config = {0};
    native.config = &config;
    owner = true;
    queued = 1; /* accepted native custom WAKE took the only queue slot */
    assert(esp_mqtt_dispatch_event(&native) == ESP_OK);
    assert(blocked_posts == 1U); /* next native event asks to wait forever */

    xt_mqtt_owner_mailbox_t box;
    xt_mqtt_mailbox_init(&box);
    assert(!xt_mqtt_mailbox_request(&box));
    const uint32_t first = xt_mqtt_mailbox_start(&box);
    assert(first && !xt_mqtt_mailbox_start(&box));
    pthread_t callers[4];
    for (unsigned i = 0; i < 4U; ++i) assert(!pthread_create(&callers[i], NULL, request, &box));
    for (unsigned i = 0; i < 4U; ++i) assert(!pthread_join(callers[i], NULL));
    /* A management ticket does not authorize a different owner epoch. */
    assert(!xt_mqtt_mailbox_take(&box, first + 4U));
    assert(!xt_mqtt_mailbox_take(&box, first | 1U));
    assert(xt_mqtt_mailbox_take(&box, first));
    ++wake_events;
    assert(!xt_mqtt_mailbox_take(&box, first));
    assert(queued == 0U);
    assert(esp_mqtt_dispatch_event(&native) == ESP_OK && blocked_posts == 1U);

    /* A callback may request the next evaluation without recursively running
     * business code or filling the native event queue. */
    assert(xt_mqtt_mailbox_request(&box) && xt_mqtt_mailbox_take(&box, first));
    ++wake_events;
    assert(xt_mqtt_mailbox_request(&box));
    xt_mqtt_mailbox_stop(&box);
    assert(!xt_mqtt_mailbox_request(&box) && !xt_mqtt_mailbox_take(&box, first));
    const uint32_t second = xt_mqtt_mailbox_start(&box);
    assert(second != first && !xt_mqtt_mailbox_take(&box, second));
    assert(xt_mqtt_mailbox_request(&box));
    assert(!xt_mqtt_mailbox_take(&box, first));
    assert(xt_mqtt_mailbox_take(&box, second));
    ++wake_events;
    xt_mqtt_mailbox_stop(&box);
    assert(wake_events == 3U && native_events == 2U && blocked_posts == 1U);
    return 0;
}
