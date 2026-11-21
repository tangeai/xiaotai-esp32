// SPDX-License-Identifier: MIT
/* Product configuration of the network engine: validation, the defaults of
 * an all-zero configuration, and the copy the engine's init resolves. Kept
 * apart from the lifecycle so that the product glue (and its test) links
 * this file alone. */
#include <stdatomic.h>
#include <string.h>
#include "internal.h"
#include "xiaotai/port/kv.h"

#define DEFAULT_SPACE "wifi_cfg"
#define DEFAULT_AP_PREFIX "XiaoTai-"
#define DEFAULT_HISTORY 5U
#define DEFAULT_FIRST_MS 1000U
#define DEFAULT_MAX_MS 30000U
/* The access point name is the prefix plus four hexadecimal digits and must
 * fit an SSID. */
#define AP_PREFIX_MAX (XIAOTAI_NET_SSID_MAX - 4U - 1U)

static xiaotai_net_config_t s_configured;
static atomic_bool s_closed;

static bool text_fits(const char *text, size_t min, size_t max)
{
    if (!text) return false;
    const size_t length = strnlen(text, max + 1U);
    return length >= min && length <= max;
}

xiaotai_err_t net_config_check(const xiaotai_net_config_t *config)
{
    if (!config) return XIAOTAI_OK;
    if (config->layout != XIAOTAI_NET_STORE_RECORD && config->layout != XIAOTAI_NET_STORE_PLAIN) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (config->space && !text_fits(config->space, 1U, XIAOTAI_KV_NAME_MAX)) return XIAOTAI_ERR_INVALID_ARG;
    /* Deployed plain-layout devices keep their credentials in a namespace
     * only the product knows. */
    if (config->layout == XIAOTAI_NET_STORE_PLAIN && !config->space) return XIAOTAI_ERR_INVALID_ARG;
    if (config->history_capacity != 0U && config->history_capacity != 4U && config->history_capacity != 5U) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (config->provisioning != XIAOTAI_NET_PROVISIONING_CONCURRENT &&
        config->provisioning != XIAOTAI_NET_PROVISIONING_EXCLUSIVE) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (config->ap_prefix && !text_fits(config->ap_prefix, 0U, AP_PREFIX_MAX)) return XIAOTAI_ERR_INVALID_ARG;

    const xiaotai_net_retry_policy_t *retry = &config->retry;
    if (retry->jitter_percent > 100U) return XIAOTAI_ERR_INVALID_ARG;
    const uint32_t first = retry->first_ms ? retry->first_ms : DEFAULT_FIRST_MS;
    const uint32_t max = retry->max_ms ? retry->max_ms : DEFAULT_MAX_MS;
    if (max < first) return XIAOTAI_ERR_INVALID_ARG;
    if ((retry->steps_ms == NULL) != (retry->step_count == 0U)) return XIAOTAI_ERR_INVALID_ARG;
    if (retry->step_count > XIAOTAI_NET_RETRY_STEPS_MAX) return XIAOTAI_ERR_INVALID_ARG;
    if (retry->step_count && retry->grow_from_previous_base) return XIAOTAI_ERR_INVALID_ARG;

    /* The serial path's short passwords are the plain layout's read rule; a
     * record-layout product never stored such a password. */
    if (config->serial_short_passwords && config->layout != XIAOTAI_NET_STORE_PLAIN) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    if (config->dns_fallback != XIAOTAI_NET_DNS_FALLBACK_SLOT_EMPTY &&
        config->dns_fallback != XIAOTAI_NET_DNS_FALLBACK_ALL_EMPTY &&
        config->dns_fallback != XIAOTAI_NET_DNS_FALLBACK_NEVER) {
        return XIAOTAI_ERR_INVALID_ARG;
    }
    return XIAOTAI_OK;
}

void net_config_resolve(const xiaotai_net_config_t *configured, xiaotai_net_config_t *effective)
{
    if (!effective) return;
    if (configured) {
        *effective = *configured;
    } else {
        memset(effective, 0, sizeof(*effective));
    }
    if (!effective->space && effective->layout == XIAOTAI_NET_STORE_RECORD) effective->space = DEFAULT_SPACE;
    if (effective->history_capacity == 0U) effective->history_capacity = DEFAULT_HISTORY;
    if (!effective->ap_prefix) effective->ap_prefix = DEFAULT_AP_PREFIX;
    xiaotai_net_retry_policy_t *retry = &effective->retry;
    if (retry->first_ms == 0U) retry->first_ms = DEFAULT_FIRST_MS;
    if (retry->max_ms == 0U) retry->max_ms = DEFAULT_MAX_MS;
    if (retry->max_credentials_ms == 0U) retry->max_credentials_ms = retry->max_ms;
}

xiaotai_err_t xiaotai_net_configure(const xiaotai_net_config_t *config)
{
    if (atomic_load(&s_closed)) return XIAOTAI_ERR_INVALID_STATE;
    const xiaotai_err_t checked = net_config_check(config);
    if (checked != XIAOTAI_OK) return checked;
    if (config) {
        s_configured = *config;
    } else {
        memset(&s_configured, 0, sizeof(s_configured));
    }
    return XIAOTAI_OK;
}

void net_config_configured(xiaotai_net_config_t *configured)
{
    if (configured) *configured = s_configured;
}

void net_config_close(void) { atomic_store(&s_closed, true); }

void net_config_reset(void)
{
    atomic_store(&s_closed, false);
    memset(&s_configured, 0, sizeof(s_configured));
}
