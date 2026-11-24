// SPDX-License-Identifier: MIT
#include <string.h>
#include "store.h"
#include "xiaotai/port/kv.h"
#include "xiaotai/port/log.h"

typedef struct {
    uint8_t version;
    xiaotai_net_credentials_t credentials;
} credentials_record_t;

_Static_assert(sizeof(xiaotai_net_credentials_t) == 98, "plain credential layout");
_Static_assert(sizeof(credentials_record_t) == 99, "versioned credential layout");

static bool plain(void) { return net_state->config.layout == XIAOTAI_NET_STORE_PLAIN; }
static const char *space(void) { return net_state->config.space; }
static const char *boot_key(void) { return plain() ? "wifi_provision" : "provision"; }

static void report_store_failure(const char *operation, xiaotai_err_t result)
{
    xiaotai_net_diag_t *diag = net_diag_edit();
    if (diag->store_failures != UINT32_MAX) ++diag->store_failures;
    net_diag_done();
    XIAOTAI_LOGW("net", "credentials %s failed (error=0x%x)", operation, (unsigned)result);
}

static bool valid(const xiaotai_net_credentials_t *credentials, bool allow_short)
{
    if (!credentials || !credentials->ssid[0] ||
        !memchr(credentials->ssid, '\0', sizeof(credentials->ssid))) return false;
    const char *end = memchr(credentials->password, '\0', sizeof(credentials->password));
    return end && (allow_short || end == credentials->password || end - credentials->password >= 8);
}

bool xiaotai_net_credentials_valid(const xiaotai_net_credentials_t *credentials)
{
    return valid(credentials, false);
}

bool net_credentials_valid_for(const xiaotai_net_credentials_t *credentials, uint32_t method)
{
    const bool allow_short = net_state && net_state->config.serial_short_passwords &&
                             (method == 0 || method == XIAOTAI_PROVISION_SERIAL);
    return valid(credentials, allow_short);
}

static void copy_canonical(xiaotai_net_credentials_t *to, const xiaotai_net_credentials_t *from)
{
    memset(to, 0, sizeof(*to));
    memcpy(to->ssid, from->ssid, strlen(from->ssid));
    memcpy(to->password, from->password, strlen(from->password));
}

xiaotai_err_t net_store_load(xiaotai_net_credentials_t *credentials)
{
    if (!credentials) return XIAOTAI_ERR_INVALID_ARG;
    memset(credentials, 0, sizeof(*credentials));
    credentials_record_t record = {0};
    const bool raw = plain();
    size_t expected = raw ? sizeof(record.credentials) : sizeof(record);
    size_t size = expected;
    xiaotai_err_t result = xiaotai_kv_get_blob_strict(space(), raw ? "wifi" : "credentials",
        raw ? (void *)&record.credentials : (void *)&record, &size, XIAOTAI_KV_WAIT_MAX_MS);
    if (result == XIAOTAI_OK) {
        if (size != expected) result = XIAOTAI_ERR_INVALID_SIZE;
        else if (!raw && record.version != 1) result = XIAOTAI_FAIL;
    } else if (!raw && result == XIAOTAI_ERR_NOT_FOUND) {
        result = xiaotai_kv_get_str(space(), "ssid", record.credentials.ssid,
                                    sizeof(record.credentials.ssid), XIAOTAI_KV_WAIT_MAX_MS);
        if (result == XIAOTAI_OK) {
            result = xiaotai_kv_get_str(space(), "password", record.credentials.password,
                                        sizeof(record.credentials.password), XIAOTAI_KV_WAIT_MAX_MS);
        }
    }

    if (result == XIAOTAI_OK) {
        if (valid(&record.credentials, raw)) copy_canonical(credentials, &record.credentials);
        else result = XIAOTAI_FAIL;
    }
    net_store_wipe(&record, sizeof(record));
    return result;
}

xiaotai_err_t net_store_save(const xiaotai_net_credentials_t *credentials)
{
    const bool raw = plain();
    if (!valid(credentials, raw)) return XIAOTAI_ERR_INVALID_ARG;
    credentials_record_t record = {.version = 1};
    copy_canonical(&record.credentials, credentials);
    const xiaotai_kv_op_t ops[] = {
        {.kind = XIAOTAI_KV_OP_SET, .key = raw ? "wifi" : "credentials",
         .data = raw ? (const void *)&record.credentials : (const void *)&record,
         .size = raw ? sizeof(record.credentials) : sizeof(record)},
        {.kind = XIAOTAI_KV_OP_ERASE, .key = boot_key()},
    };
    const xiaotai_err_t result = xiaotai_kv_commit(space(), ops, 2, XIAOTAI_KV_WAIT_MAX_MS);
    net_store_wipe(&record, sizeof(record));
    net_saved_credentials_update(result == XIAOTAI_OK);
    return result;
}

xiaotai_err_t net_store_forget(void)
{
    /* Even an error can occur after erasing. Do not race it with a history write. */
    const bool raw = plain();
    if (!raw) net_history_suspend();
    const xiaotai_err_t result = raw ? xiaotai_kv_erase(space(), "wifi", XIAOTAI_KV_WAIT_MAX_MS) :
                                     xiaotai_kv_erase_space(space(), XIAOTAI_KV_WAIT_MAX_MS);
    net_saved_credentials_update(false);
    return result;
}

xiaotai_err_t net_store_boot_flag(bool *set)
{
    if (!set) return XIAOTAI_ERR_INVALID_ARG;
    *set = false;
    uint8_t marker = 0;
    const xiaotai_err_t result = xiaotai_kv_get_u8(space(), boot_key(), &marker, XIAOTAI_KV_WAIT_MAX_MS);
    if (result == XIAOTAI_OK) *set = marker == 1;
    return result == XIAOTAI_ERR_NOT_FOUND ? XIAOTAI_OK : result;
}

xiaotai_err_t net_store_request_boot(bool forget)
{
    const uint8_t yes = 1;
    xiaotai_kv_op_t ops[4] = {{.kind = XIAOTAI_KV_OP_SET_U8, .key = boot_key(), .data = &yes}};
    size_t count = 1;
    if (forget) {
        const bool raw = plain();
        ops[count++] = (xiaotai_kv_op_t){.kind = XIAOTAI_KV_OP_ERASE, .key = raw ? "wifi" : "credentials"};
        if (!raw) {
            ops[count++] = (xiaotai_kv_op_t){.kind = XIAOTAI_KV_OP_ERASE, .key = "ssid"};
            ops[count++] = (xiaotai_kv_op_t){.kind = XIAOTAI_KV_OP_ERASE, .key = "password"};
        }
    }
    /* The boot mark is applied first, so a partial erase cannot auto-join. */
    const xiaotai_err_t result = xiaotai_kv_commit(space(), ops, count, XIAOTAI_KV_WAIT_MAX_MS);
    if (forget) net_saved_credentials_update(false);
    return result;
}

xiaotai_err_t net_store_clear_boot(void)
{
    return xiaotai_kv_erase(space(), boot_key(), XIAOTAI_KV_WAIT_MAX_MS);
}

static xiaotai_err_t change_credentials(net_call_t *call)
{
    const bool forget = call->arg != 0;
    xiaotai_net_credentials_t credentials = {0};
    if (!forget) memcpy(&credentials, call->data, sizeof(credentials));
    net_store_wipe(call->data, call->size);
    call->size = 0;

    xiaotai_err_t result = XIAOTAI_ERR_INVALID_STATE;
    if (net_state->driver_running) {
        xiaotai_net_provision_status_t provision;
        xiaotai_net_provision_status(&provision);
        if (!net_state->link.verifying && provision.state != XIAOTAI_NET_PROVISION_VERIFYING &&
            provision.state != XIAOTAI_NET_PROVISION_STORING &&
            provision.state != XIAOTAI_NET_PROVISION_STORE_FAILED) {
            result = net_gate_config_begin();
            if (result == XIAOTAI_OK) {
                result = forget ? net_store_forget() : net_store_save(&credentials);
                if (result == XIAOTAI_OK) {
                    copy_canonical(&net_state->credentials, &credentials);
                    net_state->has_credentials = !forget;
                    if (call->arg == 2) net_restart_required();
                } else report_store_failure(forget ? "erase" : "write", result);
                net_gate_config_end();
            }
        }
        net_store_wipe(&provision, sizeof(provision));
    }
    net_store_wipe(&credentials, sizeof(credentials));
    return result;
}

xiaotai_err_t xiaotai_net_set_credentials(const xiaotai_net_credentials_t *credentials)
{
    if (!net_credentials_valid_for(credentials, 0)) return XIAOTAI_ERR_INVALID_ARG;
    xiaotai_net_credentials_t canonical;
    copy_canonical(&canonical, credentials);
    const xiaotai_err_t result = net_call(change_credentials, 0, &canonical, sizeof(canonical),
                                           NULL, 0, NULL, XIAOTAI_KV_WAIT_MAX_MS);
    net_store_wipe(&canonical, sizeof(canonical));
    return result;
}

xiaotai_err_t xiaotai_net_forget_credentials(void)
{
    return net_call(change_credentials, 1, NULL, 0, NULL, 0, NULL, XIAOTAI_KV_WAIT_MAX_MS);
}

#if XIAOTAI_NET_CONSOLE_FORGET
xiaotai_err_t net_store_forget_restart(void)
{
    return net_call(change_credentials, 2, NULL, 0, NULL, 0, NULL, XIAOTAI_KV_WAIT_MAX_MS);
}
#endif

static xiaotai_err_t initial_credentials(void)
{
    const xiaotai_err_t result = net_store_load(&net_state->credentials);
    net_state->has_credentials = result == XIAOTAI_OK;
    net_saved_credentials_update(net_state->has_credentials);
    if (result != XIAOTAI_OK && result != XIAOTAI_ERR_NOT_FOUND) report_store_failure("read", result);
    /* Unusable stored credentials leave provisioning available. */
    return XIAOTAI_OK;
}

const net_unit_t net_store_unit = {.name = "store", .init = initial_credentials};
