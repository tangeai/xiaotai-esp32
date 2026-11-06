/* Operator-requested C61 APP OTA; never invoked by startup or a version mismatch. */
#include "coprocessor_update.h"
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "eh_host_event.h"
#include "eh_host_peer_data.h"
#include "eh_host_sys.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_hosted_ota.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "psa/crypto.h"
#include "starter_media.h"
#include "starter_runtime.h"
#include "starter_tirtc.h"
#include "wifi_manager.h"

#define C61_APP_LIMIT 0x180000U
#define DIAG_MESSAGE_ID 0x57545331U
static const char *TAG = "cp_ota";
static atomic_bool s_busy;
static char s_url[192];
static uint8_t s_expected_sha[32];
static bool s_diag_registered;

bool coprocessor_update_busy(void)
{
    return atomic_load(&s_busy);
}

static bool media_idle(void)
{
    return starter_runtime_status().state == STARTER_RUNTIME_WAITING &&
           !starter_media_status().active && !starter_tirtc_connected();
}

static void peer_data(void *context, esp_event_base_t base, int32_t id, void *data)
{
    (void)context;
    (void)base;
    (void)id;
    const eh_host_event_peer_data_t *message = data;
    if (!message) return;
    if (message->msg_id == DIAG_MESSAGE_ID && message->data && message->len <= 1000) {
        printf("+C61:%.*s\r\n", (int)message->len, (const char *)message->data);
        fflush(stdout);
    }
    /* Pinned Hosted 3.0.7 mallocs the posted payload. This is this project's
     * sole peer-data event consumer; release it after the synchronous read. */
    free((void *)message->data);
}

esp_err_t coprocessor_diagnostics_command(const char *command)
{
    if (strcmp(command, "INFO") && strcmp(command, "RXRESET") &&
        strcmp(command, "SINK0") && strcmp(command, "SINK1") && strcmp(command, "ROLLBACK")) return ESP_ERR_INVALID_ARG;
    bool rollback = strcmp(command, "ROLLBACK") == 0;
    if (rollback && !media_idle()) return ESP_ERR_INVALID_STATE;
    if (!s_diag_registered) {
        esp_err_t err = esp_event_handler_register(EH_HOST_EVENT, EH_HOST_EVENT_PEER_DATA_RX, peer_data, NULL);
        if (err != ESP_OK) return err;
        s_diag_registered = true;
    }
    esp_err_t err = eh_host_peer_data_send(DIAG_MESSAGE_ID, (const uint8_t *)command, strlen(command));
    if (err == ESP_OK && rollback) {
        puts("+CPROLLBACK:REQUESTED,VERIFY_AFTER_RESTART");
        fflush(stdout);
        vTaskDelay(pdMS_TO_TICKS(4000));
        esp_restart();
    }
    return err;
}

esp_err_t coprocessor_log_identity(void)
{
    eh_host_coprocessor_fwver_t version = {0};
    esp_hosted_app_desc_t description = {0};
    esp_err_t err = eh_host_sys_get_cp_fw_version(&version);
    if (err != ESP_OK) return err;
    err = eh_host_sys_get_cp_app_desc(&description);
    if (err != ESP_OK) return err;
    char sha[65];
    for (unsigned i = 0; i < 32; ++i) snprintf(sha + i * 2, 3, "%02x", description.app_elf_sha256[i]);
    printf("+CPINFO:project=%.32s,version=%.32s,idf=%.32s,elf=%s\r\n",
           description.project_name, description.version, description.idf_ver, sha);
    fflush(stdout);
    return ESP_OK;
}

static esp_err_t verify_image_sha256(const uint8_t *data, size_t size, const uint8_t expected[32])
{
    if (!data || !expected || psa_crypto_init() != PSA_SUCCESS) return ESP_ERR_INVALID_CRC;
    return psa_hash_compare(PSA_ALG_SHA_256, data, size, expected, 32) == PSA_SUCCESS
        ? ESP_OK : ESP_ERR_INVALID_CRC;
}

static esp_err_t stage_image(uint8_t **image, size_t *size)
{
    esp_http_client_config_t config = {.url = s_url, .timeout_ms = 10000,
        .buffer_size = 2048, .buffer_size_tx = 512};
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_ERR_NO_MEM;
    esp_err_t err = esp_http_client_open(client, 0);
    int64_t length = err == ESP_OK ? esp_http_client_fetch_headers(client) : -1;
    if (err == ESP_OK && (esp_http_client_get_status_code(client) != 200 ||
        length < 32 + sizeof(esp_app_desc_t) || length > C61_APP_LIMIT)) err = ESP_ERR_INVALID_SIZE;
    uint8_t *data = err == ESP_OK ? heap_caps_malloc((size_t)length, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) : NULL;
    if (err == ESP_OK && !data) err = ESP_ERR_NO_MEM;
    size_t received = 0;
    while (err == ESP_OK && received < (size_t)length) {
        int n = esp_http_client_read(client, (char *)data + received, (int)((size_t)length - received));
        if (n <= 0) err = ESP_ERR_INVALID_SIZE;
        else received += (size_t)n;
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    if (err == ESP_OK) err = verify_image_sha256(data, received, s_expected_sha);
    if (err == ESP_OK) {
        const esp_image_header_t *header = (const esp_image_header_t *)data;
        const esp_app_desc_t *app = (const esp_app_desc_t *)(data + 32);
        if (header->magic != ESP_IMAGE_HEADER_MAGIC || header->chip_id != 20 ||
            header->min_chip_rev_full > 100 || header->max_chip_rev_full < 100 ||
            app->magic_word != ESP_APP_DESC_MAGIC_WORD ||
            (strncmp(app->project_name, "xiaotai_wt99p461_c61", sizeof(app->project_name)) &&
             strncmp(app->project_name, "network_adapter", sizeof(app->project_name))))
            err = ESP_ERR_INVALID_VERSION;
    }
    if (err != ESP_OK) heap_caps_free(data);
    else { *image = data; *size = received; }
    return err;
}

static void update_task(void *argument)
{
    (void)argument;
    uint8_t *image = NULL;
    size_t length = 0, written = 0;
    esp_err_t err = media_idle() ? stage_image(&image, &length) : ESP_ERR_INVALID_STATE;
    if (err == ESP_OK && !media_idle()) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "verified C61 APP staged: bytes=%u; preserving active CP slot", (unsigned)length);
        err = esp_hosted_cp_ota_begin();
    }
    while (err == ESP_OK && written < length) {
        size_t n = length - written;
        if (n > 512) n = 512;
        err = esp_hosted_cp_ota_write(image + written, (uint32_t)n);
        if (err == ESP_OK) written += n;
        if (written && written % 65536 == 0) ESP_LOGI(TAG, "C61 OTA transferred %u/%u", (unsigned)written, (unsigned)length);
    }
    if (err == ESP_OK) err = esp_hosted_cp_ota_end();
    heap_caps_free(image);
    if (err == ESP_OK && !media_idle()) err = ESP_ERR_INVALID_STATE;
    if (err == ESP_OK) err = esp_hosted_cp_ota_activate();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "C61 OTA activated; restarting P4 after CP activation timer");
        vTaskDelay(pdMS_TO_TICKS(3000));
        esp_restart();
    }
    ESP_LOGE(TAG, "C61 OTA failed: written=%u/%u error=%s", (unsigned)written,
             (unsigned)length, esp_err_to_name(err));
    atomic_store(&s_busy, false);
    vTaskDeleteWithCaps(NULL);
}

esp_err_t coprocessor_update_start(const char *url, const char *sha256)
{
    if (!url || !sha256 || strlen(sha256) != 64 || strlen(url) >= sizeof(s_url) ||
        strncmp(url, "http://", 7) || strchr(url, '@') || strchr(url, '?')) return ESP_ERR_INVALID_ARG;
    if (!wifi_manager_connected() || !media_idle()) return ESP_ERR_INVALID_STATE;
    uint8_t digest[32];
    for (unsigned i = 0; i < 32; ++i) {
        unsigned value = 0;
        char pair[3] = {sha256[2 * i], sha256[2 * i + 1], 0};
        if (strspn(pair, "0123456789abcdefABCDEF") != 2 || sscanf(pair, "%2x", &value) != 1)
            return ESP_ERR_INVALID_ARG;
        digest[i] = (uint8_t)value;
    }
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_busy, &expected, true)) return ESP_ERR_INVALID_STATE;
    strcpy(s_url, url);
    memcpy(s_expected_sha, digest, sizeof(digest));
    if (xTaskCreateWithCaps(update_task, "cp_ota", 8192, NULL, 5, NULL,
                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        atomic_store(&s_busy, false);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
