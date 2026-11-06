#include "starter_diagnostics.h"
#include "board.h"
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "starter_tirtc.h"
#include "sdkconfig.h"
#include <cstdio>
#include <cstring>

esp_err_t starter_diagnostics_version(void)
{
    uint8_t mac[6];
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err != ESP_OK) return err;
    const auto board = xiaotai::board::SelectedBoard().identity();
    const esp_app_desc_t *app = esp_app_get_description();
    char sha[65];
    /* The IDF log helper truncates to CONFIG_APP_RETRIEVE_LEN_ELF_SHA. AT
     * identity uses the complete descriptor digest for artifact comparison. */
    for (size_t i = 0; i < sizeof(app->app_elf_sha256); ++i)
        snprintf(sha + 2 * i, 3, "%02x", app->app_elf_sha256[i]);
    printf("+VERSION: board=%s revision=%s version=%s idf=%s mac=" MACSTR "\n",
           board.model, board.revision, app->version, app->idf_ver, MAC2STR(mac));
    printf("+BUILD: elf_sha256=%s video=%d opus=%d network=%d rtc=1\n", sha,
           CONFIG_XIAOTAI_H5_VIDEO || CONFIG_XIAOTAI_AI_VISION,
           strcmp(STARTER_TIRTC_AUDIO_CODEC_NAME, "opus") == 0, CONFIG_ESP_WIFI_ENABLED);
    return ESP_OK;
}
