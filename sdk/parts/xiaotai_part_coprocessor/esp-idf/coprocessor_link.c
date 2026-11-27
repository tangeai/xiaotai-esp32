// SPDX-License-Identifier: MIT
#include "coprocessor_link.h"
#include "xiaotai/port/log.h"
#include "xiaotai/port/os.h"
#include "eh_host_core.h"
#include "eh_host_cp_ota.h"
#include "eh_host_feat_rpc.h"
#include "eh_host_feat_rpc_ext_v2_types.h"
#include "eh_host_mcu_transport_init_event.h"
#include "eh_host_sys.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include <errno.h>
#include <string.h>

static xiaotai_err_t lifecycle_error(int status)
{
    if (!status) return XIAOTAI_OK;
    if (status == -ETIMEDOUT) return XIAOTAI_ERR_TIMEOUT;
    if (status == -ENOMEM) return XIAOTAI_ERR_NO_MEM;
    return XIAOTAI_FAIL;
}

xiaotai_err_t xiaotai_coprocessor_link_wait(uint32_t timeout_ms)
{
    return eh_host_wait_auto_init_ready(timeout_ms) == 0 ? XIAOTAI_OK : XIAOTAI_ERR_TIMEOUT;
}

xiaotai_err_t xiaotai_coprocessor_link_start(uint32_t timeout_ms)
{
    /* Hosted owns the lifecycle and has fixed internal reset/connect waits
     * in the manual API. There is no caller-selectable timeout to forward. */
    (void)timeout_ms;
    int status = eh_host_init(NULL);
    if (!status) status = eh_host_connect_to_slave();
    return lifecycle_error(status);
}

xiaotai_err_t xiaotai_coprocessor_link_stop(void) { return lifecycle_error(eh_host_deinit()); }
uint16_t xiaotai_coprocessor_link_chip(void) { return eh_host_mcu_transport_get_chip_id(); }

void xiaotai_coprocessor_link_facts(xiaotai_coprocessor_link_facts_t *out)
{
    *out = (xiaotai_coprocessor_link_facts_t){
        .firmware = eh_host_mcu_transport_get_fw_version(),
        .capabilities = eh_host_mcu_transport_get_capabilities(),
        .ext_capabilities = eh_host_mcu_transport_get_ext_capabilities(),
        .rpc_version_advertised = eh_host_mcu_transport_peer_advertised_rpc_version(),
        .bus_buffer_config = eh_host_mcu_transport_sdio_buf_config() != NULL,
    };
}

xiaotai_err_t xiaotai_coprocessor_link_version(xiaotai_coprocessor_version_t *out)
{
    eh_host_coprocessor_fwver_t info = {0};
    const esp_err_t err = eh_host_sys_get_cp_fw_version(&info);
    if (err == ESP_OK) *out = (xiaotai_coprocessor_version_t){info.major1, info.minor1, info.patch1};
    return err;
}

xiaotai_err_t xiaotai_coprocessor_link_app(xiaotai_coprocessor_app_t *out)
{
    esp_hosted_app_desc_t info = {0};
    const esp_err_t err = eh_host_sys_get_cp_app_desc(&info);
    if (err != ESP_OK) return err;
    /* Pristine Hosted 3.0.7 synthesizes an APP version from the protocol
     * version and leaves the project empty. Do not report it as identity. */
    if (!info.project_name[0] || !info.version[0]) return ESP_ERR_INVALID_RESPONSE;
    out->magic = info.magic_word;
    memcpy(out->project, info.project_name, sizeof(out->project));
    memcpy(out->version, info.version, sizeof(out->version));
    memcpy(out->platform, info.idf_ver, sizeof(out->platform));
    memcpy(out->elf_sha256, info.app_elf_sha256, sizeof(out->elf_sha256));
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_coprocessor_link_probe_unversioned(bool *matched)
{
    *matched = false;
    eh_rpc_ctrl_cmd_t *req = eh_rpc_ctrl_cmd_alloc();
    if (!req) return XIAOTAI_ERR_NO_MEM;
    req->u.wifi_mac.mode = WIFI_IF_STA;
    void *reply = NULL;
    /* The synchronous Hosted helper owns and frees the request. */
    if (eh_host_feat_rpc_request_sync(RPC_ID__Req_GetMACAddress, req, &reply) != 0 || !reply) return XIAOTAI_FAIL;
    eh_rpc_ctrl_cmd_t *resp = reply;
    *matched = resp->resp_event_status == ESP_ERR_WIFI_NOT_INIT;
    eh_rpc_ctrl_cmd_free(resp);
    return XIAOTAI_OK;
}

/* These Hosted calls send RPCs; the peer performs the Flash operations. */
xiaotai_err_t xiaotai_coprocessor_link_ota_begin(void) { return eh_host_cp_ota_begin(); }
xiaotai_err_t xiaotai_coprocessor_link_ota_end(void) { return eh_host_cp_ota_end(); }
xiaotai_err_t xiaotai_coprocessor_link_ota_activate(void) { return eh_host_cp_ota_activate(); }

xiaotai_err_t xiaotai_coprocessor_link_ota_write(const uint8_t *data, size_t size)
{
    if (!data || !size) return XIAOTAI_ERR_INVALID_SIZE;
#if SIZE_MAX > UINT32_MAX
    if (size > UINT32_MAX) return XIAOTAI_ERR_INVALID_SIZE;
#endif
    return eh_host_cp_ota_write(data, (uint32_t)size);
}

#if CONFIG_ESP_HOSTED_HOST_FEAT_PEER_DATA_READY
#include "eh_host_event.h"
#include "eh_host_peer_data.h"
#include "esp_event.h"
#include <stdlib.h>

#define REPLY_TIMEOUT_MS 1000U
#define REPLY_MAX_BYTES 1000U
/* Protocol identifier of the shipped diagnostic firmware, not a board ID.
 * INFO may send an alloc_trace= prelude before its final elf= record. */
#define DIAGNOSTIC_TEXT_CHANNEL 0x57545331U

/* Only the network worker sends a command; the event loop delivers replies.
 * The pointer is borrowed only while pending and under the mutex. */
static xiaotai_mutex_t s_mutex;
static xiaotai_signal_t s_signal;
static bool s_registered;
static struct {
    bool pending, received, ambiguous;
    uint32_t channel;
    char *out;
    size_t capacity, prelude;
    xiaotai_err_t result;
} s_reply;

static bool prefix(const eh_host_event_peer_data_t *msg, const char *text, size_t n)
{
    return msg->data && msg->len >= n && !memcmp(msg->data, text, n);
}

static void append_terminal(const eh_host_event_peer_data_t *msg)
{
    if (!s_reply.capacity) return;
    const size_t max = s_reply.capacity - 1U;
    const size_t n = msg->len < max ? msg->len : max;
    size_t kept = 0;
    if (s_reply.prelude && max - n > 1U) {
        const size_t room = max - n - 1U;
        const size_t trace = s_reply.prelude < room ? s_reply.prelude : room;
        memmove(s_reply.out + n + 1U, s_reply.out, trace);
        s_reply.out[n] = '\n';
        kept = trace + 1U;
    }
    memcpy(s_reply.out, msg->data, n);
    s_reply.out[n + kept] = '\0';
}

static void peer_data(void *ctx, esp_event_base_t base, int32_t id, void *data)
{
    (void)ctx; (void)base; (void)id;
    const eh_host_event_peer_data_t *msg = data;
    if (!msg) return;
    xiaotai_mutex_lock(&s_mutex, XIAOTAI_WAIT_FOREVER);
    if (s_reply.pending && !s_reply.received && msg->msg_id == s_reply.channel) {
        if (prefix(msg, "elf=", sizeof("elf=") - 1U)) {
            s_reply.received = true;
            s_reply.result = msg->len <= REPLY_MAX_BYTES ? XIAOTAI_OK : XIAOTAI_ERR_INVALID_SIZE;
            if (s_reply.result == XIAOTAI_OK) append_terminal(msg);
            xiaotai_signal_give(&s_signal);
        } else if (s_reply.capacity && msg->len <= REPLY_MAX_BYTES &&
                   prefix(msg, "alloc_trace=", sizeof("alloc_trace=") - 1U)) {
            const size_t n = msg->len < s_reply.capacity ? msg->len : s_reply.capacity - 1U;
            memcpy(s_reply.out, msg->data, n);
            s_reply.prelude = n;
        }
    }
    xiaotai_mutex_unlock(&s_mutex);
    if (msg->msg_id == DIAGNOSTIC_TEXT_CHANNEL &&
        prefix(msg, "rollback=ERROR_NO_VERIFIED_PREVIOUS_IMAGE", sizeof("rollback=ERROR_NO_VERIFIED_PREVIOUS_IMAGE") - 1U)) {
        XIAOTAI_LOGW("coprocessor", "peer rejected rollback: no verified previous image");
    }
    /* Pinned 3.0.7 posts an allocated copy with this event. As in V2 this
     * is its sole consumer, including when no request waits for the payload. */
    free((void *)msg->data);
}

static xiaotai_err_t ensure_handler(void)
{
    if (s_registered) return XIAOTAI_OK;
    xiaotai_err_t err = xiaotai_mutex_init(&s_mutex);
    if (err != XIAOTAI_OK) return err;
    err = xiaotai_signal_init(&s_signal);
    if (err != XIAOTAI_OK) {
        xiaotai_mutex_deinit(&s_mutex);
        return err;
    }
    err = esp_event_handler_register(EH_HOST_EVENT, EH_HOST_EVENT_PEER_DATA_RX, peer_data, NULL);
    if (err != ESP_OK) {
        xiaotai_signal_deinit(&s_signal);
        xiaotai_mutex_deinit(&s_mutex);
        return err;
    }
    s_registered = true;
    return XIAOTAI_OK;
}

xiaotai_err_t xiaotai_coprocessor_link_command(uint32_t channel, const char *command, bool wait_for_reply,
                                              char *out, size_t capacity)
{
    if (wait_for_reply && channel != DIAGNOSTIC_TEXT_CHANNEL) return XIAOTAI_ERR_NOT_SUPPORTED;
    xiaotai_err_t err = ensure_handler();
    if (err != XIAOTAI_OK) return err;
    if (wait_for_reply) {
        /* An event may arrive before the send RPC returns. Arm first. */
        xiaotai_mutex_lock(&s_mutex, XIAOTAI_WAIT_FOREVER);
        if (s_reply.ambiguous) {
            xiaotai_mutex_unlock(&s_mutex);
            return XIAOTAI_ERR_INVALID_STATE;
        }
        (void)xiaotai_signal_take(&s_signal, 0);
        s_reply.pending = true;
        s_reply.received = false;
        s_reply.channel = channel;
        s_reply.out = out;
        s_reply.capacity = capacity;
        s_reply.prelude = 0;
        xiaotai_mutex_unlock(&s_mutex);
    }
    err = eh_host_peer_data_send(channel, (const uint8_t *)command, strlen(command));
    if (!wait_for_reply) return err;
    if (err == ESP_OK) (void)xiaotai_signal_take(&s_signal, REPLY_TIMEOUT_MS);
    xiaotai_mutex_lock(&s_mutex, XIAOTAI_WAIT_FOREVER);
    if (!s_reply.received) {
        /* The wire carries no request identity. Do not let a late event
         * after a timeout satisfy some later command on this channel. */
        s_reply.ambiguous = true;
        if (err == ESP_OK) err = XIAOTAI_ERR_TIMEOUT;
    } else if (err == ESP_OK) {
        err = s_reply.result;
    }
    if (err != ESP_OK && capacity) out[0] = '\0';
    s_reply.pending = false;
    s_reply.out = NULL;
    xiaotai_mutex_unlock(&s_mutex);
    return err;
}

#else

xiaotai_err_t xiaotai_coprocessor_link_command(uint32_t channel, const char *command, bool wait_for_reply,
                                              char *out, size_t capacity)
{
    (void)channel; (void)command; (void)wait_for_reply; (void)out; (void)capacity;
    return XIAOTAI_ERR_NOT_SUPPORTED;
}

#endif
