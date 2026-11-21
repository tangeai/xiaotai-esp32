// SPDX-License-Identifier: MIT
#include <limits.h>
#include <string.h>
#include <strings.h>

#include "tirtc_internal.h"
#include "xiaotai/hal.h"

/* The worker is the sole caller of the SDK lifecycle. */
static bool s_initialized;

static const char *service_endpoint(const xiaotai_rtc_start_t *config)
{
    const char *endpoint = config->endpoint;
    if (!endpoint || !*endpoint) return NULL;
    if (config->endpoint_default_unset &&
        (!strcmp(endpoint, "http://ep-tirtc.tange365.com") || !strcmp(endpoint, "https://ep-tirtc.tange365.com")))
        return NULL;
    if (TIRTC_SDK_SSL_SUPPORT || !config->endpoint_http_rewrite) return endpoint;
    /* Never rewrite an explicit port, non-root path, query or similar host. */
    static const char *const aliases[][2] = {
        {"https://ep-tirtc.tange365.com", "http://ep-tirtc.tange365.com"},
        {"https://ep-tirtc.tange365.com/", "http://ep-tirtc.tange365.com/"},
        {"https://ep-pre-tirtc.tange365.com", "http://ep-pre-tirtc.tange365.com"},
        {"https://ep-pre-tirtc.tange365.com/", "http://ep-pre-tirtc.tange365.com/"},
    };
    for (size_t i = 0; i < sizeof(aliases) / sizeof(aliases[0]); ++i)
        if (!strcasecmp(endpoint, aliases[i][0])) return aliases[i][1];
    return endpoint;
}

static int string_option(TIRTCOPTION option, const char *value, bool without_nul)
{
    if (!value || !*value) return 0;
    const size_t length = strlen(value);
    if (length > UINT32_MAX - 1U) return TIRTC_E_INVALID_PARAMETER;
    return TiRtcSetOption(option, value, (uint32_t)length + (without_nul ? 0U : 1U));
}

xiaotai_err_t xt_tirtc_status(int result)
{
    switch (result) {
    case 0: return XIAOTAI_OK;
    case TIRTC_E_INVALID_PARAMETER: return XIAOTAI_ERR_INVALID_ARG;
    case TIRTC_E_NOT_INITIALIZED:
    case TIRTC_E_INVALID_HANDLE: return XIAOTAI_ERR_INVALID_STATE;
    case TIRTC_E_TIMEOUTED:
    case TIRTC_E_CONN_TIMEOUTCLOSE:
    case TIRTC_E_HTTP_TIMEOUT: return XIAOTAI_ERR_TIMEOUT;
    case TIRTC_E_LACK_OF_RESOURCE:
    case TIRTC_E_HTTP_NOMEM: return XIAOTAI_ERR_NO_MEM;
    case TIRTC_E_BUSY: return XIAOTAI_ERR_BUSY;
    default: return XIAOTAI_FAIL;
    }
}

static xiaotai_err_t with_detail(int result, int *detail)
{
    if (detail) *detail = result;
    return xt_tirtc_status(result);
}

xiaotai_err_t xiaotai_rtc_start(const xiaotai_rtc_start_t *config, int *detail)
{
    if (s_initialized) return with_detail(TIRTC_E_BUSY, detail);
    if (!config || !config->device_id || !*config->device_id || !config->device_secret ||
        !*config->device_secret || !config->links || config->network_kind > XIAOTAI_NETWORK_CELLULAR)
        return with_detail(TIRTC_E_INVALID_PARAMETER, detail);
    if (config->media_transport == XIAOTAI_TRANSPORT_ON && !XT_TIRTC_MEDIA_TRANSPORT) {
        if (detail) *detail = 0;
        return XIAOTAI_ERR_NOT_SUPPORTED;
    }
    xt_tirtc_log_configure(config->stack_log_level);
    const uint32_t buffer = config->send_buffer_bytes;
    const int connections = config->links;
    const int cache = config->connect_cache == XIAOTAI_TRANSPORT_ON;
    int rc = buffer ? TiRtcSetOption(TIRTC_OPT_MAX_SEND_BUFFER, &buffer, sizeof(buffer)) : 0;
    if (!rc) rc = TiRtcSetOption(TIRTC_OPT_MAX_CONNECTIONS, &connections, sizeof(connections));
    if (!rc && config->connect_cache != XIAOTAI_TRANSPORT_STACK_DEFAULT)
        rc = TiRtcSetOption(TIRTC_OPT_CONNECT_CACHE, &cache, sizeof(cache));
    if (rc) return with_detail(rc, detail);

#if XT_TIRTC_MEDIA_TRANSPORT
    if (config->media_transport == XIAOTAI_TRANSPORT_ON && config->media_transport_before_init)
        tgtrp_enable_media_transport();
#endif
    rc = TiRtcInit();
    if (rc) return with_detail(rc, detail);
    s_initialized = true;
#if XT_TIRTC_MEDIA_TRANSPORT
    if (config->media_transport == XIAOTAI_TRANSPORT_ON && !config->media_transport_before_init)
        tgtrp_enable_media_transport();
#endif
    const bool without_nul = config->option_strings_without_nul;
    rc = string_option(TIRTC_OPT_SERVICE_ENDPOINT, service_endpoint(config), without_nul);
    if (!rc) rc = string_option(TIRTC_OPT_DEVICE_SECRET_KEY, config->device_secret, without_nul);
    if (!rc) rc = string_option(TIRTC_OPT_CLIENT_ID, config->client_id, without_nul);
    const int network = config->network_kind == XIAOTAI_NETWORK_CELLULAR ? TIRTC_NETCONN_4G : TIRTC_NETCONN_WIFI;
    if (!rc) rc = TiRtcSetOption(TIRTC_OPT_NETWORK_TYPE, &network, sizeof(network));
    if (!rc && config->poll_timeout_ms) {
        const int poll = config->poll_timeout_ms;
        const int applied = TiRtcSetOption(TIRTC_OPT_TGTRP_POLL_TIMEOUT, &poll, sizeof(poll));
        if (applied < 0) rc = applied; /* A positive effective timeout is not an error. */
    }
    if (!rc) {
#if defined(XIAOTAI_TIRTC_IDLE_STACK_POLICY)
        rc = xt_tirtc_start_with_idle_stack(config->device_id, &xt_tirtc_callbacks, config->vendor_idle_stack_bytes);
#else
        rc = TiRtcStart(config->device_id, &xt_tirtc_callbacks);
#endif
    }
    if (rc) xiaotai_rtc_uninit();
    return with_detail(rc, detail);
}

xiaotai_err_t xiaotai_rtc_stop(int *detail)
{
    return with_detail(s_initialized ? TiRtcStop() : TIRTC_E_NOT_INITIALIZED, detail);
}

void xiaotai_rtc_uninit(void)
{
    if (!s_initialized) return;
    TiRtcUninit();
    s_initialized = false;
}
