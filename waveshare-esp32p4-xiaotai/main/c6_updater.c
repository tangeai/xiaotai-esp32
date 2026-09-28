#include "c6_updater.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "eh_host_core.h"
#include "eh_host_cp_ota.h"
#include "eh_host_feat_rpc.h"
#include "eh_host_feat_rpc_ext_v2_types.h"
#include "eh_host_mcu_transport_init_event.h"
#include "eh_host_sys.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_store.h"
#include "starter_product.h"

/* C6 APP image based on ESP-Hosted 3.0.7 with sequential OTA erasure.
 * It is not a 4 MB flash image. */
extern const uint8_t c6_app_start[] asm("_binary_c6_app_bin_start");
extern const uint8_t c6_app_end[] asm("_binary_c6_app_bin_end");

#define C6_CHIP_ID 0x0dU
#define C6_OTA_SLOT_MIN_BYTES 0x1c0000U
#define C6_OTA_CHUNK_BYTES 1024U
#define C6_LEGACY_OTA_SLOT_BYTES 0x180000U
#define C6_HANDSHAKE_TIMEOUT_MS 15000U
#define C6_NVS_TIMEOUT_MS 3000U

static const char *TAG = "c6_update";
static bool s_transfer_started_this_boot;
typedef struct {
    uint32_t hosted_major;
    uint32_t hosted_minor;
    uint32_t hosted_patch;
    char project[32];
    char version[32];
    uint8_t elf_sha256[32];
    size_t minimum_proven_ota_bytes;
    bool end_activates;
} c6_approved_source_t;

static const c6_approved_source_t s_approved_sources[] = {{
    .hosted_major = 3, .hosted_minor = 0, .hosted_patch = 7,
    .project = "eh_cp_wifi_sta", .version = "3.0.7-xiaotai.2",
    .elf_sha256 = {
        0x15, 0xcc, 0x26, 0x97, 0x81, 0x86, 0x86, 0xa6,
        0xa7, 0xa4, 0x12, 0x0a, 0xf6, 0x45, 0x3b, 0xc8,
        0xbd, 0x66, 0x30, 0x95, 0xe6, 0x0e, 0x0c, 0x3a,
        0xd1, 0x60, 0x67, 0x7e, 0x1a, 0xbf, 0xf2, 0x43,
    },
    .minimum_proven_ota_bytes = 0x10f760U,
}, {
    .hosted_major = 3, .hosted_minor = 0, .hosted_patch = 7,
    .project = "eh_cp_wifi_sta", .version = "3.0.7-xiaotai.3",
    .elf_sha256 = {
        0x56, 0xf9, 0x3a, 0xc0, 0xba, 0x85, 0xa4, 0x31,
        0x6d, 0xcf, 0x67, 0x37, 0x8c, 0xde, 0x14, 0xd8,
        0xcf, 0x0a, 0xe3, 0x70, 0xbb, 0x33, 0xc8, 0x12,
        0x70, 0x02, 0x68, 0x17, 0xd3, 0xe8, 0x5a, 0xaf,
    },
    .minimum_proven_ota_bytes = 0x10f760U,
}, {
    .hosted_major = 3, .hosted_minor = 0, .hosted_patch = 7,
    .project = "eh_cp_wifi_sta", .version = "3.0.7-xiaotai.4",
    .elf_sha256 = {
        0x1e, 0xca, 0x43, 0xdb, 0x04, 0xc7, 0x35, 0xcc,
        0x50, 0x5f, 0xe9, 0xd0, 0x5e, 0xbb, 0x73, 0xcf,
        0x62, 0x11, 0x56, 0xc1, 0xdf, 0x83, 0x1c, 0x44,
        0x4b, 0xa2, 0x8a, 0xe9, 0x04, 0xc3, 0xab, 0x03,
    },
    .minimum_proven_ota_bytes = 0x10f760U,
}};

static const c6_approved_source_t *c6_approved_source(
    const eh_host_coprocessor_fwver_t *hosted,
    const esp_hosted_app_desc_t *running, size_t image_size)
{
    if (running->magic_word != ESP_APP_DESC_MAGIC_WORD) return NULL;
    for (size_t i = 0; i < sizeof(s_approved_sources) / sizeof(s_approved_sources[0]); ++i) {
        const c6_approved_source_t *source = &s_approved_sources[i];
        if (source->minimum_proven_ota_bytes < image_size ||
            source->minimum_proven_ota_bytes == 0 ||
            hosted->major1 != source->hosted_major ||
            hosted->minor1 != source->hosted_minor ||
            hosted->patch1 != source->hosted_patch ||
            strcmp(running->project_name, source->project) != 0 ||
            strcmp(running->version, source->version) != 0 ||
            memcmp(running->app_elf_sha256, source->elf_sha256,
                   sizeof(source->elf_sha256)) != 0) continue;
        return source;
    }
    return NULL;
}

static const uint8_t s_image_sha256[32] = {
    0x87, 0x00, 0xa7, 0x21, 0x5c, 0x77, 0xff, 0xc9,
    0x81, 0x0a, 0xb2, 0x7d, 0xbf, 0x76, 0x87, 0xd6,
    0x22, 0x2c, 0x62, 0x0c, 0x13, 0xd7, 0x81, 0x4d,
    0xc5, 0x02, 0xac, 0xe6, 0xa7, 0x84, 0xb4, 0xf5,
};

static bool c6_target_matches(const esp_hosted_app_desc_t *running,
                              const esp_app_desc_t *target)
{
    if (!memchr(running->project_name, 0, sizeof(running->project_name)) ||
        !memchr(running->version, 0, sizeof(running->version))) return false;
    return running->magic_word == ESP_APP_DESC_MAGIC_WORD &&
           strcmp(running->project_name, target->project_name) == 0 &&
           strcmp(running->version, target->version) == 0 &&
           memcmp(running->app_elf_sha256, target->app_elf_sha256,
                  sizeof(target->app_elf_sha256)) == 0;
}

static bool c6_known_legacy_compatible(const eh_host_coprocessor_fwver_t *hosted,
                                       const esp_hosted_app_desc_t *running)
{
    /* This legacy C6 reports only three descriptor strings. It ran with the
     * same Hosted protocol before the updater was introduced, but cannot be
     * authenticated as an OTA source without its ELF hash. Leave it running. */
    return hosted->major1 == 3 && hosted->minor1 == 0 && hosted->patch1 == 7 &&
           running->magic_word == 0 &&
           strcmp(running->project_name, "eh_cp_wifi_sta") == 0 &&
           strcmp(running->version, "1") == 0;
}

static esp_err_t c6_validate_image(esp_app_desc_t *target, size_t *image_size)
{
    size_t size = (size_t)(c6_app_end - c6_app_start);
    if (size < sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) +
                   sizeof(*target) || size > C6_OTA_SLOT_MIN_BYTES)
        return ESP_ERR_INVALID_SIZE;

    mbedtls_sha256_context context;
    uint8_t digest[32];
    mbedtls_sha256_init(&context);
    int rc = mbedtls_sha256_starts(&context, 0);
    for (size_t offset = 0; rc == 0 && offset < size; offset += 4096U) {
        size_t count = size - offset;
        if (count > 4096U) count = 4096U;
        rc = mbedtls_sha256_update(&context, c6_app_start + offset, count);
        if ((offset & 0x7fffU) == 0) vTaskDelay(1);
    }
    if (rc == 0) rc = mbedtls_sha256_finish(&context, digest);
    mbedtls_sha256_free(&context);
    if (rc != 0 || memcmp(digest, s_image_sha256, sizeof(digest)) != 0)
        return ESP_ERR_INVALID_CRC;
    const esp_image_header_t *header = (const void *)c6_app_start;
    if (header->magic != ESP_IMAGE_HEADER_MAGIC ||
        header->chip_id != C6_CHIP_ID || header->segment_count == 0)
        return ESP_ERR_INVALID_RESPONSE;
    memcpy(target, c6_app_start + sizeof(*header) +
                   sizeof(esp_image_segment_header_t), sizeof(*target));
    if (target->magic_word != ESP_APP_DESC_MAGIC_WORD ||
        !memchr(target->project_name, 0, sizeof(target->project_name)) ||
        !memchr(target->version, 0, sizeof(target->version)) ||
        strcmp(target->project_name, "eh_cp_wifi_sta") != 0 ||
        strcmp(target->version, "3.0.7-xiaotai.5") != 0)
        return ESP_ERR_INVALID_RESPONSE;
    *image_size = size;
    return ESP_OK;
}

static esp_err_t c6_read_attempt(uint8_t *attempt)
{
    size_t size = sizeof(*attempt);
    esp_err_t err = nvs_store_read("c6_update", "attempt", NVS_STORE_GET_U8,
                                   attempt, &size, C6_NVS_TIMEOUT_MS);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *attempt = 0;
        return ESP_OK;
    }
    return err;
}

static esp_err_t c6_write_attempt(bool attempted)
{
    nvs_store_op_t op = attempted
        ? (nvs_store_op_t){.kind = NVS_STORE_U8, .key = "attempt",
                           .value = &(const uint8_t){1}, .size = 1}
        : (nvs_store_op_t){.kind = NVS_STORE_ERASE_KEY, .key = "attempt"};
    return nvs_store_execute("c6_update", &op, 1, C6_NVS_TIMEOUT_MS);
}

static bool c6_has_legacy_ota_contract(void)
{
    /* ESP-Hosted before RPC 350 advertised these init capabilities but not
     * its version. This is a protocol-family gate, not an APP identity. */
    if (eh_host_mcu_transport_get_fw_version() != 0 ||
        eh_host_mcu_transport_get_capabilities() != 0x0d ||
        eh_host_mcu_transport_get_ext_capabilities() != 0 ||
        eh_host_mcu_transport_peer_advertised_rpc_version() ||
        eh_host_mcu_transport_sdio_buf_config() != NULL) return false;

    /* V2 RPC 257 must be answered by the old CP, even though Wi-Fi is not
     * initialized yet. A timeout is not evidence of a compatible OTA peer. */
    eh_rpc_ctrl_cmd_t *req = eh_rpc_ctrl_cmd_alloc();
    if (!req) return false;
    req->u.wifi_mac.mode = WIFI_IF_STA;
    eh_rpc_ctrl_cmd_t *reply = NULL;
    if (eh_host_feat_rpc_request_sync(RPC_ID__Req_GetMACAddress,
                                      req, (void **)&reply) != 0) return false;
    bool matched = reply->resp_event_status == ESP_ERR_WIFI_NOT_INIT;
    ESP_LOGI(TAG, "legacy RPC 257 status=0x%04x match=%u",
             (unsigned)reply->resp_event_status, (unsigned)matched);
    eh_rpc_ctrl_cmd_free(reply);
    return matched;
}

static esp_err_t c6_read_identity(eh_host_coprocessor_fwver_t *hosted,
                                  esp_hosted_app_desc_t *app,
                                  bool *legacy_ota)
{
    *legacy_ota = false;
    if (eh_host_mcu_transport_get_chip_id() != C6_CHIP_ID)
        return ESP_ERR_INVALID_RESPONSE;
    esp_err_t err = eh_host_sys_get_cp_fw_version(hosted);
    if (err != ESP_OK) {
        if (!c6_has_legacy_ota_contract()) return err;
        /* The pre-version-RPC Hosted protocol also predates APP descriptor
         * RPC 267. Do not mistake a different, partially broken CP for it. */
        esp_hosted_app_desc_t probe = {0};
        if (eh_host_sys_get_cp_app_desc(&probe) == ESP_OK)
            return ESP_ERR_INVALID_RESPONSE;
        *legacy_ota = true;
        ESP_LOGI(TAG, "legacy C6 OTA protocol accepted: no version/descriptor RPC");
        return ESP_OK;
    }
    memset(app, 0, sizeof(*app));
    /* APP descriptor is an independent RPC; never infer it from Hosted semver. */
    err = eh_host_sys_get_cp_app_desc(app);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "C6 APP descriptor RPC failed (msg=267): %s",
                 esp_err_to_name(err));
        return err;
    }
    if (!memchr(app->project_name, 0, sizeof(app->project_name)) ||
        !memchr(app->version, 0, sizeof(app->version)))
        return ESP_ERR_INVALID_RESPONSE;
    return ESP_OK;
}

static esp_err_t c6_stream_image(size_t image_size, bool old_end_activates,
                                 bool *activation_may_have_started)
{
    *activation_may_have_started = false;
    esp_err_t err = eh_host_cp_ota_begin();
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "OTA begin ok; bytes=%u", (unsigned)image_size);
    for (size_t offset = 0; offset < image_size; offset += C6_OTA_CHUNK_BYTES) {
        size_t count = image_size - offset;
        if (count > C6_OTA_CHUNK_BYTES) count = C6_OTA_CHUNK_BYTES;
        err = eh_host_cp_ota_write(c6_app_start + offset, (uint32_t)count);
        if (err != ESP_OK) {
            /* A timed-out write may already have been appended by C6. Do not
             * resend this chunk and do not call end as a cancellation API. */
            ESP_LOGE(TAG, "write result uncertain at offset=%u: %s",
                     (unsigned)offset, esp_err_to_name(err));
            return err;
        }
        starter_product_set_c6_update(STARTER_C6_UPDATE_WRITING,
            (uint8_t)(5U + (offset + count) * 85U / image_size), ESP_OK);
        vTaskDelay(1);
    }
    ESP_LOGI(TAG, "OTA writes complete; bytes=%u", (unsigned)image_size);
    /* On some C6 builds end commits and restarts before the RPC reply. */
    *activation_may_have_started = old_end_activates;
    err = eh_host_cp_ota_end();
    if (err != ESP_OK) return err;
    ESP_LOGI(TAG, "OTA image validated by C6");
    if (!old_end_activates) {
        starter_product_set_c6_update(STARTER_C6_UPDATE_ACTIVATING, 94, ESP_OK);
        *activation_may_have_started = true;
        err = eh_host_cp_ota_activate();
    }
    return err;
}

esp_err_t c6_updater_run(bool manual_retry)
{
    starter_product_set_c6_update(STARTER_C6_UPDATE_CHECKING, 0, ESP_OK);
    if (eh_host_wait_auto_init_ready(C6_HANDSHAKE_TIMEOUT_MS) != 0) {
        starter_product_set_c6_update(STARTER_C6_UPDATE_RECOVERY, 0, ESP_ERR_TIMEOUT);
        return ESP_ERR_TIMEOUT;
    }
    starter_product_set_c6_update(STARTER_C6_UPDATE_VERIFYING, 1, ESP_OK);
    esp_app_desc_t target;
    size_t image_size = 0;
    esp_err_t err = c6_validate_image(&target, &image_size);
    if (err != ESP_OK) goto failed;
    eh_host_coprocessor_fwver_t hosted = {0};
    esp_hosted_app_desc_t running = {0};
    bool legacy_ota = false;
    err = c6_read_identity(&hosted, &running, &legacy_ota);
    if (err != ESP_OK) goto failed;
    uint8_t attempt = 0;
    err = c6_read_attempt(&attempt);
    if (err != ESP_OK) goto failed;
    if (c6_target_matches(&running, &target) &&
        hosted.major1 == 3 && hosted.minor1 == 0 && hosted.patch1 == 7) {
        if (attempt) err = c6_write_attempt(false);
        if (err != ESP_OK) goto failed;
        ESP_LOGI(TAG, "C6 APP %s verified%s; OTA skipped", target.version,
                 attempt ? " after activation" : "");
        starter_product_set_c6_update(STARTER_C6_UPDATE_READY, 100, ESP_OK);
        return ESP_OK;
    }
    if (c6_known_legacy_compatible(&hosted, &running) && !attempt &&
        !s_transfer_started_this_boot) {
        ESP_LOGW(TAG, "C6 legacy APP identity is incomplete; compatible Hosted 3.0.7, OTA skipped");
        starter_product_set_c6_update(STARTER_C6_UPDATE_READY, 100, ESP_OK);
        return ESP_OK;
    }
    if (s_transfer_started_this_boot) {
        err = ESP_ERR_INVALID_STATE;
        goto failed;
    }

    const c6_approved_source_t *source = legacy_ota ? NULL :
        c6_approved_source(&hosted, &running, image_size);
    if ((legacy_ota && image_size > C6_LEGACY_OTA_SLOT_BYTES) ||
        (!legacy_ota && !source)) {
        err = ESP_ERR_NOT_SUPPORTED;
        goto failed;
    }
    ESP_LOGI(TAG, "C6 OTA source=%s image=%u", legacy_ota ? "legacy-v2" :
             running.version, (unsigned)image_size);
    if (attempt && !manual_retry) {
        err = ESP_ERR_INVALID_STATE;
        goto failed;
    }
    err = c6_write_attempt(true);
    if (err != ESP_OK) goto failed;
    starter_product_set_c6_update(STARTER_C6_UPDATE_WRITING, 5, ESP_OK);
    s_transfer_started_this_boot = true;
    bool activation_may_have_started = false;
    err = c6_stream_image(image_size, legacy_ota || source->end_activates,
                          &activation_may_have_started);
    if (err != ESP_OK && !activation_may_have_started) goto failed;
    /* The C6 activation reboots its SDIO endpoint. Restart the P4 before
     * the transport watchdog sees that expected link loss; the next boot
     * verifies the target descriptor before clearing the attempt marker. */
    starter_product_set_c6_update(STARTER_C6_UPDATE_HANDSHAKING, 96, ESP_OK);
    ESP_LOGI(TAG, "OTA activation submitted rc=%s; restarting host to re-handshake",
             esp_err_to_name(err));
    /* Older C6 schedules its own restart five seconds after OTA end. Do not
     * reset the host while it is still running the old image. */
    vTaskDelay(pdMS_TO_TICKS(legacy_ota ? 6000 : 50));
    esp_restart();
    return ESP_ERR_INVALID_STATE;

failed:
    ESP_LOGE(TAG, "C6 update blocked: %s", esp_err_to_name(err));
    starter_product_set_c6_update(s_transfer_started_this_boot
        ? STARTER_C6_UPDATE_RECOVERY_POWER_CYCLE : STARTER_C6_UPDATE_RECOVERY,
        0, err);
    return err;
}
