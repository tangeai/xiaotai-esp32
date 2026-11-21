// SPDX-License-Identifier: MIT
#include <string.h>
#include "platform_internal.h"
#include "xiaotai/port/log.h"

typedef struct {
    platform_service_t services[64];
    char *arena;
    size_t count, used, capacity;
    unsigned invalid;
} discovery_t;

static const platform_service_t *find(const discovery_t *d, const char *name)
{
    for (size_t i = 0; i < d->count; ++i)
        if (!strcmp(d->services[i].name, name)) return &d->services[i];
    return NULL;
}

static bool required(const char *name)
{
    if (!strcmp(name, XIAOTAI_PLATFORM_SERVICE_DEVICE) || !strcmp(name, XIAOTAI_PLATFORM_SERVICE_MQTT)) return true;
    for (size_t i = 0; i < platform.required_count; ++i)
        if (!strcmp(name, platform.required[i])) return true;
    return false;
}

static char *append(discovery_t *d, const char *value)
{
    const size_t size = strlen(value) + 1U;
    if (size > d->capacity - d->used) return NULL;
    char *at = d->arena + d->used;
    memcpy(at, value, size);
    d->used += size;
    return at;
}

static xiaotai_err_t service(const char *key, size_t key_size, const char *value, size_t value_size, void *ctx)
{
    discovery_t *d = ctx;
    char name[XIAOTAI_PLATFORM_SERVICE_NAME_MAX + 4];
    if (platform_json_text_read(key, key_size, name, sizeof(name)) != XIAOTAI_OK) return XIAOTAI_OK;
    const size_t n = strlen(name);
    if (n <= 4U || strcmp(name + n - 4U, "-srv")) return XIAOTAI_OK;
    name[n - 4U] = '\0';
    if (!platform_service_name_valid(name) || find(d, name)) return XIAOTAI_OK; /* Deployed first-key lookup. */
    if (d->count == sizeof(d->services) / sizeof(d->services[0])) return XIAOTAI_ERR_INVALID_SIZE;
    char url[XIAOTAI_PLATFORM_URL_MAX];
    xiaotai_err_t err = platform_json_text_read(value, value_size, url, sizeof(url));
    if (err == XIAOTAI_OK) {
        err = !strcmp(name, XIAOTAI_PLATFORM_SERVICE_MQTT) ?
            platform_url_mqtt(url, platform.config.mqtt_address_unchecked, url, sizeof(url)) :
            platform_url_http(url, true, url, sizeof(url));
    }
    if (err != XIAOTAI_OK) {
        if (required(name)) return XIAOTAI_ERR_PLATFORM_BAD_SERVICE;
        ++d->invalid;
        XIAOTAI_LOGW("platform", "optional discovery service %s rejected", name);
    }
    char *stored_name = append(d, name);
    char *stored_url = err == XIAOTAI_OK ? append(d, url) : NULL;
    if (!stored_name || (err == XIAOTAI_OK && !stored_url)) return XIAOTAI_ERR_INVALID_SIZE;
    d->services[d->count++] = (platform_service_t){.name = stored_name, .base = stored_url};
    return XIAOTAI_OK;
}

xiaotai_err_t platform_discover(uint32_t revision)
{
    char url[XIAOTAI_PLATFORM_URL_MAX];
    xiaotai_err_t err = platform_environment_discovery_url(url, sizeof(url));
    if (err != XIAOTAI_OK) return err;
    const xiaotai_http_request_t req = {.method = XIAOTAI_HTTP_GET, .url = url,
        .timeout_ms = XIAOTAI_PLATFORM_TIMEOUT_DEFAULT_MS};
    platform_setup_reply_t reply = {0};
    err = platform_session_http(revision, &req, &reply);
    if (err == XIAOTAI_OK && (reply.response.status != 200 || reply.response.redirect)) err = XIAOTAI_FAIL;
    if (err == XIAOTAI_OK) {
        discovery_t d = {.capacity = reply.size + 1U};
        d.arena = xiaotai_mem_alloc(XIAOTAI_MEM_BULK, d.capacity);
        err = d.arena ? platform_json_members(reply.body, reply.size, service, &d) : XIAOTAI_ERR_NO_MEM;
        if (err == XIAOTAI_OK && (!find(&d, XIAOTAI_PLATFORM_SERVICE_DEVICE) || !find(&d, XIAOTAI_PLATFORM_SERVICE_MQTT)))
            err = XIAOTAI_ERR_PLATFORM_BAD_SERVICE;
        for (size_t i = 0; err == XIAOTAI_OK && i < platform.required_count; ++i)
            if (!find(&d, platform.required[i])) err = XIAOTAI_ERR_PLATFORM_BAD_SERVICE;
        if (err == XIAOTAI_OK) err = platform_services_discovered(d.services, d.count, revision);
        if (d.invalid) {
            platform_lock();
            platform.stats.services_invalid += d.invalid;
            platform_unlock();
        }
        xiaotai_mem_free(d.arena);
    }
    if (err != XIAOTAI_OK && err != XIAOTAI_ERR_PLATFORM_STALE)
        XIAOTAI_LOGW("platform", "discovery failed: result=%d http=%d", err, reply.response.status);
    platform_setup_reply_release(&reply, false);
    return err;
}
