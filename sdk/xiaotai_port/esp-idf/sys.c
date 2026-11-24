// SPDX-License-Identifier: MIT
/* xiaotai/port/sys.h on ESP-IDF. */
#include "xiaotai/port/sys.h"

#include <stdio.h>
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_random.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "xiaotai/port/log.h"

void xiaotai_firmware(xiaotai_firmware_t *info)
{
    if (!info) return;
    static char platform[40];
    if (!platform[0]) (void)snprintf(platform, sizeof(platform), "ESP-IDF %s", esp_get_idf_version());
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    const esp_app_desc_t *desc = esp_app_get_description();
    *info = (xiaotai_firmware_t){
        .version = desc->version,
        .platform = platform,
        .chip = CONFIG_IDF_TARGET,
        .cores = chip.cores,
        .chip_revision = chip.revision,
    };
    /* The IDF getter uses a configured display length. Read the complete
     * descriptor with cache available; esptool patches these bytes after link. */
    static const char hex[] = "0123456789abcdef";
    const volatile uint8_t *sha = desc->app_elf_sha256;
    _Static_assert(sizeof(info->elf_sha256) == 2 * sizeof(desc->app_elf_sha256) + 1, "ELF SHA-256 size");
    for (size_t i = 0; i < sizeof(desc->app_elf_sha256); ++i) {
        const uint8_t byte = sha[i];
        info->elf_sha256[2 * i] = hex[byte >> 4];
        info->elf_sha256[2 * i + 1] = hex[byte & 0xf];
    }
    info->elf_sha256[sizeof(info->elf_sha256) - 1] = '\0';
}

uint32_t xiaotai_random(void) { return esp_random(); }

void xiaotai_restart(void)
{
    /* Drivers and connections are torn down inside esp_restart(); what they
     * print on the way is expected. */
    xiaotai_log_quiet_for_restart();
    esp_restart();
}
