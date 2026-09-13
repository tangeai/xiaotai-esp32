#include "device_binding.h"

#include <string.h>
#include <time.h>

#include "app_memory_policy.h"
#include "binding_mqtt_client.h"
#include "device_binding_http.h"
#include "device_identity.h"
#include "esp_attr.h"
#include "esp_check.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "app_task_affinity.h"
#include "platform_nvs_async.h"

static const char *TAG = "device_binding";

#define DEVICE_BINDING_TASK_STACK_SIZE 8192
#define DEVICE_BINDING_TASK_PRIORITY   2
#define DEVICE_BINDING_DEFAULT_WAIT_MS 180000U
#define DEVICE_BINDING_PENDING_NVS_NAMESPACE "bind"
#define DEVICE_BINDING_PENDING_NVS_KEY       "pending"
#define DEVICE_BINDING_PENDING_MAGIC         0x42444E50U
#define DEVICE_BINDING_PENDING_VERSION       2U
#define DEVICE_BINDING_REPORT_RETRY_MAX_MS   300000U
#define DEVICE_BINDING_WEAK_NET_RETRY_MAX_MS 60000U
#define DEVICE_BINDING_WEAK_NET_RETRY_MS     5000U
#define DEVICE_BINDING_MQTT_READY_TIMEOUT_MS 25000U
#define DEVICE_BINDING_WAIT_POLL_MS          500U
#define DEVICE_BINDING_REASON_MAX_LEN        32

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t reserved;
    int64_t expires_at_unix;
    char mac[DEVICE_BINDING_MAC_MAX_LEN];
    char code[DEVICE_BINDING_HTTP_CODE_MAX_LEN];
    char temp_client_id[DEVICE_BINDING_HTTP_CLIENT_ID_MAX_LEN];
    char temp_token[DEVICE_BINDING_HTTP_TOKEN_MAX_LEN];
} device_binding_pending_store_t;

typedef struct {
    device_binding_config_t config;
    SemaphoreHandle_t lock;
    TaskHandle_t task;
    uint32_t generation;
    uint32_t task_generation;
    bool restart_requested;
    device_binding_snapshot_t snapshot;
    char reason[DEVICE_BINDING_REASON_MAX_LEN];
    char restart_reason[DEVICE_BINDING_REASON_MAX_LEN];
} device_binding_runtime_t;

static EXT_RAM_BSS_ATTR device_binding_runtime_t s_binding;

typedef struct {
    const char *code;
    bool ready;
    uint32_t generation;
} device_binding_mqtt_ready_ctx_t;

typedef struct {
    uint32_t generation;
} device_binding_cancel_ctx_t;

typedef struct {
    device_binding_identity_t identity;
    device_binding_credentials_t existing;
    device_binding_http_report_result_t report;
    binding_mqtt_auth_grant_t grant;
} device_binding_run_workspace_t;

static esp_err_t device_binding_clear_pending_session(void);

static uint32_t device_binding_active_generation(void)
{
    uint32_t generation = 0;

    if (s_binding.lock == NULL) {
        return generation;
    }
    xSemaphoreTake(s_binding.lock, portMAX_DELAY);
    generation = s_binding.task_generation != 0U ?
                 s_binding.task_generation :
                 s_binding.generation;
    xSemaphoreGive(s_binding.lock);
    return generation;
}

static bool device_binding_cancelled(uint32_t generation)
{
    bool cancelled = false;

    if (s_binding.lock == NULL || generation == 0U) {
        return false;
    }
    xSemaphoreTake(s_binding.lock, portMAX_DELAY);
    cancelled = s_binding.generation != generation;
    xSemaphoreGive(s_binding.lock);
    return cancelled;
}

static bool device_binding_cancel_cb(void *ctx)
{
    const device_binding_cancel_ctx_t *cancel_ctx = (const device_binding_cancel_ctx_t *)ctx;

    return cancel_ctx != NULL && device_binding_cancelled(cancel_ctx->generation);
}

static esp_err_t device_binding_delay_or_cancel(uint32_t generation, uint32_t delay_ms)
{
    uint32_t elapsed_ms = 0;

    while (elapsed_ms < delay_ms) {
        uint32_t remaining_ms = delay_ms - elapsed_ms;
        uint32_t wait_ms = remaining_ms < DEVICE_BINDING_WAIT_POLL_MS ?
                           remaining_ms :
                           DEVICE_BINDING_WAIT_POLL_MS;
        if (device_binding_cancelled(generation)) {
            return ESP_ERR_INVALID_STATE;
        }
        vTaskDelay(pdMS_TO_TICKS(wait_ms));
        elapsed_ms += wait_ms;
    }
    return device_binding_cancelled(generation) ? ESP_ERR_INVALID_STATE : ESP_OK;
}

static void device_binding_set_state(device_binding_state_t state,
                                     esp_err_t last_error,
                                     const char *message)
{
    if (s_binding.lock == NULL) {
        return;
    }

    xSemaphoreTake(s_binding.lock, portMAX_DELAY);
    s_binding.snapshot.state = state;
    s_binding.snapshot.running = s_binding.task != NULL;
    s_binding.snapshot.last_error = last_error;
    if (message != NULL) {
        strlcpy(s_binding.snapshot.message, message, sizeof(s_binding.snapshot.message));
    }
    xSemaphoreGive(s_binding.lock);
}

static void device_binding_set_identity(const device_binding_identity_t *identity)
{
    if (s_binding.lock == NULL || identity == NULL) {
        return;
    }

    xSemaphoreTake(s_binding.lock, portMAX_DELAY);
    strlcpy(s_binding.snapshot.mac, identity->mac, sizeof(s_binding.snapshot.mac));
    xSemaphoreGive(s_binding.lock);
}

static void device_binding_set_code(const char *code)
{
    if (s_binding.lock == NULL) {
        return;
    }

    xSemaphoreTake(s_binding.lock, portMAX_DELAY);
    strlcpy(s_binding.snapshot.code, code != NULL ? code : "", sizeof(s_binding.snapshot.code));
    xSemaphoreGive(s_binding.lock);
}

static void device_binding_set_device_id(const char *device_id)
{
    if (s_binding.lock == NULL) {
        return;
    }

    xSemaphoreTake(s_binding.lock, portMAX_DELAY);
    strlcpy(s_binding.snapshot.device_id,
            device_id != NULL ? device_id : "",
            sizeof(s_binding.snapshot.device_id));
    xSemaphoreGive(s_binding.lock);
}

static bool device_binding_pending_store_valid(const device_binding_pending_store_t *store,
                                               const device_binding_identity_t *identity,
                                               time_t now)
{
    return store != NULL && identity != NULL &&
           store->magic == DEVICE_BINDING_PENDING_MAGIC &&
           store->version == DEVICE_BINDING_PENDING_VERSION &&
           store->mac[sizeof(store->mac) - 1] == '\0' &&
           store->code[sizeof(store->code) - 1] == '\0' &&
           store->temp_client_id[sizeof(store->temp_client_id) - 1] == '\0' &&
           store->temp_token[sizeof(store->temp_token) - 1] == '\0' &&
           strcmp(store->mac, identity->mac) == 0 &&
           store->code[0] != '\0' &&
           store->temp_client_id[0] != '\0' &&
           store->temp_token[0] != '\0' &&
           store->expires_at_unix > (int64_t)now;
}

static esp_err_t device_binding_clear_pending_session(void)
{
    return platform_nvs_async_erase_key_and_wait(DEVICE_BINDING_PENDING_NVS_NAMESPACE,
                                                  DEVICE_BINDING_PENDING_NVS_KEY);
}

static esp_err_t device_binding_save_pending_session(const device_binding_identity_t *identity,
                                                     const device_binding_http_report_result_t *report,
                                                     uint32_t wait_timeout_ms)
{
    time_t now = 0;
    device_binding_pending_store_t store = {
        .magic = DEVICE_BINDING_PENDING_MAGIC,
        .version = DEVICE_BINDING_PENDING_VERSION,
    };

    if (identity == NULL || report == NULL ||
        report->code[0] == '\0' ||
        report->temp_client_id[0] == '\0' ||
        report->temp_token[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    time(&now);
    store.expires_at_unix = (int64_t)now + (int64_t)((wait_timeout_ms + 999U) / 1000U);
    strlcpy(store.mac, identity->mac, sizeof(store.mac));
    strlcpy(store.code, report->code, sizeof(store.code));
    strlcpy(store.temp_client_id, report->temp_client_id, sizeof(store.temp_client_id));
    strlcpy(store.temp_token, report->temp_token, sizeof(store.temp_token));

    esp_err_t ret = platform_nvs_async_set_blob_and_wait(DEVICE_BINDING_PENDING_NVS_NAMESPACE,
                                                          DEVICE_BINDING_PENDING_NVS_KEY,
                                                          &store,
                                                          sizeof(store));
    if (ret == ESP_OK) {
        ESP_LOGI(TAG,
                 "binding pending session saved: code=%s temp_client=%s expires_in=%us",
                 store.code,
                 store.temp_client_id,
                 (unsigned)((wait_timeout_ms + 999U) / 1000U));
    }
    return ret;
}

static bool device_binding_load_pending_session(const device_binding_identity_t *identity,
                                                device_binding_http_report_result_t *report)
{
    device_binding_pending_store_t store = {0};
    size_t store_len = sizeof(store);
    time_t now = 0;

    if (identity == NULL || report == NULL) {
        return false;
    }
    esp_err_t ret = platform_nvs_async_get_blob(DEVICE_BINDING_PENDING_NVS_NAMESPACE,
                                                 DEVICE_BINDING_PENDING_NVS_KEY,
                                                 &store,
                                                 &store_len);
    if (ret != ESP_OK) {
        return false;
    }
    if (store_len != sizeof(store)) {
        (void)device_binding_clear_pending_session();
        return false;
    }

    time(&now);
    if (!device_binding_pending_store_valid(&store, identity, now)) {
        (void)device_binding_clear_pending_session();
        return false;
    }

    memset(report, 0, sizeof(*report));
    report->type = DEVICE_BINDING_HTTP_REPORT_UNBOUND;
    strlcpy(report->code, store.code, sizeof(report->code));
    strlcpy(report->temp_client_id, store.temp_client_id, sizeof(report->temp_client_id));
    strlcpy(report->temp_token, store.temp_token, sizeof(report->temp_token));
    ESP_LOGI(TAG,
             "binding pending session restored: code=%s temp_client=%s ttl_left=%llds",
             store.code,
             store.temp_client_id,
             (long long)(store.expires_at_unix - (int64_t)now));
    return true;
}

static esp_err_t device_binding_apply_credentials(const char *device_id,
                                                  const char *device_key,
                                                  const char *reason)
{
    if (s_binding.config.save_credentials == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t ret = s_binding.config.save_credentials(device_id, device_key, s_binding.config.ctx);
    if (ret == ESP_OK) {
        (void)device_binding_clear_pending_session();
        device_binding_set_code("");
        device_binding_set_device_id(device_id);
        device_binding_set_state(DEVICE_BINDING_STATE_BOUND, ESP_OK, "device bound");
        ESP_LOGI(TAG, "binding credentials applied");
    } else {
        ESP_LOGW(TAG, "binding credentials apply failed: %s", esp_err_to_name(ret));
    }
    return ret;
}

static bool device_binding_load_existing_credentials(device_binding_credentials_t *credentials)
{
    if (credentials == NULL || s_binding.config.load_credentials == NULL) {
        return false;
    }

    memset(credentials, 0, sizeof(*credentials));
    esp_err_t ret = s_binding.config.load_credentials(credentials, s_binding.config.ctx);
    if (ret != ESP_OK ||
        credentials->device_id[0] == '\0' ||
        credentials->device_key[0] == '\0') {
        return false;
    }
    return true;
}

static bool device_binding_report_error_recoverable(esp_err_t ret)
{
    switch (ret) {
    case ESP_ERR_TIMEOUT:
    case ESP_ERR_HTTP_CONNECT:
    case ESP_ERR_HTTP_WRITE_DATA:
    case ESP_ERR_HTTP_FETCH_HEADER:
    case ESP_ERR_HTTP_CONNECTING:
    case ESP_ERR_HTTP_EAGAIN:
    case ESP_ERR_HTTP_CONNECTION_CLOSED:
    case ESP_ERR_HTTP_READ_TIMEOUT:
    case ESP_ERR_HTTP_INCOMPLETE_DATA:
        return true;
    default:
        return false;
    }
}

static bool device_binding_mqtt_wait_error_recoverable(esp_err_t ret, bool mqtt_ready)
{
    if (ret == ESP_ERR_TIMEOUT) {
        return !mqtt_ready;
    }
    return ret == ESP_FAIL ||
           ret == ESP_ERR_INVALID_STATE ||
           ret == ESP_ERR_TIMEOUT;
}

static void device_binding_on_mqtt_ready(void *ctx)
{
    device_binding_mqtt_ready_ctx_t *ready = (device_binding_mqtt_ready_ctx_t *)ctx;

    if (ready == NULL || ready->code == NULL || ready->code[0] == '\0' ||
        device_binding_cancelled(ready->generation)) {
        return;
    }
    ready->ready = true;
    device_binding_set_code(ready->code);
    device_binding_set_state(DEVICE_BINDING_STATE_WAITING_USER, ESP_OK, "waiting user bind");
    ESP_LOGI(TAG, "binding verification code ready: mqtt subscribed");
}

static esp_err_t device_binding_run(device_binding_run_workspace_t *workspace)
{
    device_binding_identity_t *identity = NULL;
    device_binding_credentials_t *existing = NULL;
    device_binding_http_report_result_t *report = NULL;
    binding_mqtt_auth_grant_t *grant = NULL;
    device_binding_mqtt_ready_ctx_t mqtt_ready = {0};
    uint32_t wait_timeout_ms = s_binding.config.wait_timeout_ms != 0U ?
                               s_binding.config.wait_timeout_ms :
                               DEVICE_BINDING_DEFAULT_WAIT_MS;
    uint32_t report_retry_elapsed_ms = 0;
    uint32_t weak_net_retry_elapsed_ms = 0;
    uint32_t mqtt_retry_elapsed_ms = 0;
    bool has_existing_credentials = false;
    bool has_pending_session = false;
    esp_err_t ret = ESP_OK;
    device_binding_cancel_ctx_t cancel_ctx = {
        .generation = device_binding_active_generation(),
    };

    if (workspace == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    identity = &workspace->identity;
    existing = &workspace->existing;
    report = &workspace->report;
    grant = &workspace->grant;

    if (!s_binding.config.enabled) {
        device_binding_set_state(DEVICE_BINDING_STATE_DISABLED, ESP_OK, "binding disabled");
        return ESP_OK;
    }
    if (device_binding_cancelled(cancel_ctx.generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_binding.config.api_base == NULL || s_binding.config.api_base[0] == '\0') {
        device_binding_set_state(DEVICE_BINDING_STATE_DISABLED, ESP_OK, "binding api empty");
        return ESP_OK;
    }
    if (s_binding.config.mqtt_uri == NULL || s_binding.config.mqtt_uri[0] == '\0') {
        device_binding_set_state(DEVICE_BINDING_STATE_ERROR, ESP_ERR_INVALID_ARG, "binding mqtt uri empty");
        return ESP_ERR_INVALID_ARG;
    }

    ret = device_identity_get(identity);
    if (ret != ESP_OK) {
        device_binding_set_state(DEVICE_BINDING_STATE_ERROR, ret, "identity failed");
        return ret;
    }
    if (device_binding_cancelled(cancel_ctx.generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    device_binding_set_identity(identity);

    has_existing_credentials = device_binding_load_existing_credentials(existing);
    if (has_existing_credentials) {
        device_binding_set_device_id(existing->device_id);
        ESP_LOGI(TAG,
                 "binding existing credentials loaded: device_id_len=%u",
                 (unsigned)strlen(existing->device_id));
    }

retry_binding_session:
    if (device_binding_cancelled(cancel_ctx.generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    has_pending_session = device_binding_load_pending_session(identity, report);
    while (!has_pending_session) {
        device_binding_set_state(DEVICE_BINDING_STATE_REPORTING, ESP_OK, "request binding code");
        memset(report, 0, sizeof(*report));
        ret = device_binding_http_report(s_binding.config.api_base,
                                         identity->mac,
                                         has_existing_credentials ? existing->device_id : NULL,
                                         has_existing_credentials ? existing->device_key : NULL,
                                         report);
        if (device_binding_cancelled(cancel_ctx.generation)) {
            return ESP_ERR_INVALID_STATE;
        }
        if (ret != ESP_OK) {
            if (device_binding_report_error_recoverable(ret) &&
                weak_net_retry_elapsed_ms < DEVICE_BINDING_WEAK_NET_RETRY_MAX_MS) {
                uint32_t delay_ms = DEVICE_BINDING_WEAK_NET_RETRY_MS;
                if (weak_net_retry_elapsed_ms + delay_ms > DEVICE_BINDING_WEAK_NET_RETRY_MAX_MS) {
                    delay_ms = DEVICE_BINDING_WEAK_NET_RETRY_MAX_MS - weak_net_retry_elapsed_ms;
                }
                device_binding_set_state(DEVICE_BINDING_STATE_REPORTING, ret, "network retry");
                ESP_LOGW(TAG,
                         "binding report network retry: ret=%s wait_ms=%u elapsed_ms=%u",
                         esp_err_to_name(ret),
                         (unsigned)delay_ms,
                         (unsigned)weak_net_retry_elapsed_ms);
                ret = device_binding_delay_or_cancel(cancel_ctx.generation, delay_ms);
                if (ret != ESP_OK) {
                    return ret;
                }
                weak_net_retry_elapsed_ms += delay_ms;
                continue;
            }
            device_binding_set_state(DEVICE_BINDING_STATE_ERROR, ret, "report failed");
            return ret;
        }
        weak_net_retry_elapsed_ms = 0;
        if (report->type == DEVICE_BINDING_HTTP_REPORT_RETRY_AFTER) {
            uint32_t delay_ms = report->retry_after_sec != 0U ?
                                report->retry_after_sec * 1000U :
                                DEVICE_BINDING_DEFAULT_WAIT_MS;
            device_binding_set_state(DEVICE_BINDING_STATE_WAITING_USER, ESP_OK, "verify pending");
            ESP_LOGI(TAG,
                     "binding report pending: service_code=%d retry_after=%us elapsed_ms=%u",
                     report->service_code,
                     (unsigned)report->retry_after_sec,
                     (unsigned)report_retry_elapsed_ms);
            if (delay_ms == 0U ||
                report_retry_elapsed_ms + delay_ms > DEVICE_BINDING_REPORT_RETRY_MAX_MS) {
                return ESP_ERR_TIMEOUT;
            }
            ret = device_binding_delay_or_cancel(cancel_ctx.generation, delay_ms);
            if (ret != ESP_OK) {
                return ret;
            }
            report_retry_elapsed_ms += delay_ms;
            continue;
        }
        /*
         * Signed rebind keeps the pre-burned device_id/device_key in flash.
         * The server may send an empty auth_grant payload, using the retained
         * credentials as proof instead of issuing a fresh key.
         */
        ret = device_binding_save_pending_session(identity, report, wait_timeout_ms);
        if (device_binding_cancelled(cancel_ctx.generation)) {
            (void)device_binding_clear_pending_session();
            return ESP_ERR_INVALID_STATE;
        }
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "binding pending session save failed: %s", esp_err_to_name(ret));
        }
        break;
    }

retry_binding_mqtt:
    if (device_binding_cancelled(cancel_ctx.generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    device_binding_set_code("");
    device_binding_set_state(DEVICE_BINDING_STATE_REPORTING, ESP_OK, "binding mqtt connect");
    ESP_LOGI(TAG,
             "binding verification code pending: wait mqtt subscribe temp_client=%s",
             report->temp_client_id);

    mqtt_ready = (device_binding_mqtt_ready_ctx_t) {
        .code = report->code,
        .ready = false,
        .generation = cancel_ctx.generation,
    };
    binding_mqtt_client_config_t mqtt_config = {
        .broker_uri = s_binding.config.mqtt_uri,
        .mac = identity->mac,
        .temp_client_id = report->temp_client_id,
        .temp_token = report->temp_token,
        .wait_timeout_ms = wait_timeout_ms,
        .ready_timeout_ms = DEVICE_BINDING_MQTT_READY_TIMEOUT_MS,
        .ready_cb = device_binding_on_mqtt_ready,
        .ready_ctx = &mqtt_ready,
        .should_cancel = device_binding_cancel_cb,
        .cancel_ctx = &cancel_ctx,
    };

    memset(grant, 0, sizeof(*grant));
    ret = binding_mqtt_client_wait_auth_grant(&mqtt_config, grant);
    if (device_binding_cancelled(cancel_ctx.generation)) {
        return ESP_ERR_INVALID_STATE;
    }
    if (ret != ESP_OK) {
        bool code_was_visible = mqtt_ready.ready;
        device_binding_set_code("");
        uint32_t attempt_budget_ms = code_was_visible ? 0U : DEVICE_BINDING_MQTT_READY_TIMEOUT_MS;
        if (device_binding_mqtt_wait_error_recoverable(ret, code_was_visible) &&
            mqtt_retry_elapsed_ms + attempt_budget_ms < DEVICE_BINDING_WEAK_NET_RETRY_MAX_MS) {
            uint32_t delay_ms = DEVICE_BINDING_WEAK_NET_RETRY_MS;
            uint32_t used_ms = mqtt_retry_elapsed_ms + attempt_budget_ms;
            bool reuse_session = !code_was_visible;
            if (used_ms + delay_ms > DEVICE_BINDING_WEAK_NET_RETRY_MAX_MS) {
                delay_ms = DEVICE_BINDING_WEAK_NET_RETRY_MAX_MS - used_ms;
            }
            device_binding_set_state(DEVICE_BINDING_STATE_REPORTING, ret, "mqtt retry");
            ESP_LOGW(TAG,
                     "binding mqtt retry: ret=%s ready=%d reuse_session=%d wait_ms=%u elapsed_ms=%u",
                     esp_err_to_name(ret),
                     code_was_visible ? 1 : 0,
                     reuse_session ? 1 : 0,
                     (unsigned)delay_ms,
                     (unsigned)used_ms);
            ret = device_binding_delay_or_cancel(cancel_ctx.generation, delay_ms);
            if (ret != ESP_OK) {
                return ret;
            }
            mqtt_retry_elapsed_ms = used_ms + delay_ms;
            if (reuse_session) {
                goto retry_binding_mqtt;
            }
            (void)device_binding_clear_pending_session();
            memset(report, 0, sizeof(*report));
            goto retry_binding_session;
        }
        if (code_was_visible) {
            (void)device_binding_clear_pending_session();
        }
        device_binding_set_state(DEVICE_BINDING_STATE_ERROR, ret, "auth grant wait failed");
        ESP_LOGW(TAG,
                 "binding auth grant wait failed: %s ready=%d",
                 esp_err_to_name(ret),
                 code_was_visible ? 1 : 0);
        return ret;
    }
    mqtt_retry_elapsed_ms = 0;

    if (grant->has_credentials) {
        if (device_binding_cancelled(cancel_ctx.generation)) {
            return ESP_ERR_INVALID_STATE;
        }
        return device_binding_apply_credentials(grant->device_id, grant->device_key, "mqtt-auth-grant");
    }

    if (has_existing_credentials) {
        if (device_binding_cancelled(cancel_ctx.generation)) {
            return ESP_ERR_INVALID_STATE;
        }
        ESP_LOGI(TAG, "binding auth grant uses retained device credentials");
        return device_binding_apply_credentials(existing->device_id,
                                                existing->device_key,
                                                "mqtt-auth-grant-retained");
    }

    /* Report is unauthenticated and intentionally never returns credentials.
     * An empty grant is valid only for a device retaining pre-burned creds. */
    ESP_LOGW(TAG, "binding auth grant missing credentials for an unprovisioned device");
    device_binding_set_state(DEVICE_BINDING_STATE_ERROR,
                             ESP_ERR_INVALID_RESPONSE,
                             "auth grant empty");
    return ESP_ERR_INVALID_RESPONSE;
}

static void device_binding_task(void *arg)
{
    (void)arg;
    device_binding_run_workspace_t *workspace =
        app_memory_calloc_psram(1, sizeof(*workspace));
    bool restart_requested = false;
    char restart_reason[DEVICE_BINDING_REASON_MAX_LEN] = {0};

    esp_err_t ret = workspace != NULL ? device_binding_run(workspace) : ESP_ERR_NO_MEM;
    free(workspace);
    if (ret == ESP_ERR_NO_MEM) {
        device_binding_set_state(DEVICE_BINDING_STATE_ERROR, ret, "binding workspace allocation failed");
    }
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "binding task failed: %s", esp_err_to_name(ret));
    }

    xSemaphoreTake(s_binding.lock, portMAX_DELAY);
    restart_requested = s_binding.restart_requested && s_binding.config.enabled;
    if (restart_requested) {
        strlcpy(restart_reason,
                s_binding.restart_reason[0] != '\0' ? s_binding.restart_reason : "restart",
                sizeof(restart_reason));
    }
    s_binding.restart_requested = false;
    s_binding.restart_reason[0] = '\0';
    s_binding.task = NULL;
    s_binding.task_generation = 0;
    s_binding.snapshot.running = false;
    xSemaphoreGive(s_binding.lock);

    if (restart_requested) {
        ESP_LOGI(TAG, "binding task restarting: reason=%s", restart_reason);
        (void)device_binding_start_async(restart_reason);
    }
    vTaskDeleteWithCaps(NULL);
}

esp_err_t device_binding_init(const device_binding_config_t *config)
{
    device_binding_credentials_t existing = {0};
    bool credentials_restored = false;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    if (config->enabled && config->load_credentials != NULL) {
        esp_err_t load_ret = config->load_credentials(&existing, config->ctx);
        credentials_restored = load_ret == ESP_OK &&
                               existing.device_id[0] != '\0' &&
                               existing.device_key[0] != '\0';
        if (load_ret != ESP_OK && load_ret != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "binding credentials restore failed: %s", esp_err_to_name(load_ret));
        }
    }

    if (s_binding.lock == NULL) {
        s_binding.lock = xSemaphoreCreateMutexWithCaps(APP_SYNC_CAPS_CONTROL);
        if (s_binding.lock == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }

    xSemaphoreTake(s_binding.lock, portMAX_DELAY);
    s_binding.config = *config;
    s_binding.task = NULL;
    s_binding.generation = 1U;
    s_binding.task_generation = 0;
    s_binding.restart_requested = false;
    s_binding.reason[0] = '\0';
    s_binding.restart_reason[0] = '\0';
    memset(&s_binding.snapshot, 0, sizeof(s_binding.snapshot));
    s_binding.snapshot.state = !config->enabled ?
                               DEVICE_BINDING_STATE_DISABLED :
                               (credentials_restored ? DEVICE_BINDING_STATE_BOUND : DEVICE_BINDING_STATE_IDLE);
    if (credentials_restored) {
        strlcpy(s_binding.snapshot.device_id,
                existing.device_id,
                sizeof(s_binding.snapshot.device_id));
    }
    strlcpy(s_binding.snapshot.message,
            !config->enabled ?
            "binding disabled" :
            (credentials_restored ? "binding credentials restored" : "binding idle"),
            sizeof(s_binding.snapshot.message));
    xSemaphoreGive(s_binding.lock);
    if (credentials_restored) {
        ESP_LOGI(TAG,
                 "binding credentials restored: device_id_len=%u",
                 (unsigned)strlen(existing.device_id));
    }
    return ESP_OK;
}

esp_err_t device_binding_start_async(const char *reason)
{
    if (s_binding.lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_binding.lock, portMAX_DELAY);
    if (!s_binding.config.enabled ||
        s_binding.config.api_base == NULL ||
        s_binding.config.api_base[0] == '\0') {
        xSemaphoreGive(s_binding.lock);
        return ESP_ERR_INVALID_STATE;
    }
    if (s_binding.task != NULL) {
        if (s_binding.generation != s_binding.task_generation) {
            s_binding.restart_requested = true;
            strlcpy(s_binding.restart_reason,
                    reason != NULL ? reason : "restart",
                    sizeof(s_binding.restart_reason));
            strlcpy(s_binding.snapshot.message,
                    "binding restart queued",
                    sizeof(s_binding.snapshot.message));
            ESP_LOGI(TAG, "binding restart queued: reason=%s", s_binding.restart_reason);
        }
        xSemaphoreGive(s_binding.lock);
        return ESP_OK;
    }
    s_binding.restart_requested = false;
    s_binding.restart_reason[0] = '\0';
    strlcpy(s_binding.reason, reason != NULL ? reason : "manual", sizeof(s_binding.reason));
    s_binding.snapshot.running = true;
    s_binding.task_generation = s_binding.generation;
    BaseType_t task_ret = xTaskCreateWithCaps(device_binding_task,
                                              "device_binding",
                                              DEVICE_BINDING_TASK_STACK_SIZE,
                                              NULL,
                                              DEVICE_BINDING_TASK_PRIORITY,
                                              &s_binding.task,
                                              APP_TASK_STACK_CAPS_INTERNAL);
    if (task_ret != pdPASS) {
        s_binding.task = NULL;
        s_binding.task_generation = 0;
        s_binding.snapshot.running = false;
        xSemaphoreGive(s_binding.lock);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "binding task started: reason=%s", s_binding.reason);
    xSemaphoreGive(s_binding.lock);
    return ESP_OK;
}

void device_binding_reset_state(const char *reason)
{
    if (s_binding.lock == NULL) {
        return;
    }

    xSemaphoreTake(s_binding.lock, portMAX_DELAY);
    s_binding.generation++;
    if (s_binding.generation == 0U) {
        s_binding.generation = 1U;
    }
    s_binding.snapshot.state = s_binding.config.enabled ?
                               DEVICE_BINDING_STATE_IDLE :
                               DEVICE_BINDING_STATE_DISABLED;
    s_binding.snapshot.running = s_binding.task != NULL;
    s_binding.snapshot.code[0] = '\0';
    s_binding.snapshot.device_id[0] = '\0';
    s_binding.snapshot.last_error = ESP_OK;
    strlcpy(s_binding.snapshot.message,
            reason != NULL ? reason : "binding reset",
            sizeof(s_binding.snapshot.message));
    xSemaphoreGive(s_binding.lock);

    esp_err_t ret = device_binding_clear_pending_session();
    if (ret != ESP_OK) {
        ESP_LOGW(TAG,
                 "binding pending session clear failed during reset: %s",
                 esp_err_to_name(ret));
    }
}

void device_binding_get_snapshot(device_binding_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    if (s_binding.lock == NULL) {
        snapshot->state = DEVICE_BINDING_STATE_DISABLED;
        strlcpy(snapshot->message, "binding not initialized", sizeof(snapshot->message));
        return;
    }

    xSemaphoreTake(s_binding.lock, portMAX_DELAY);
    *snapshot = s_binding.snapshot;
    xSemaphoreGive(s_binding.lock);
}
