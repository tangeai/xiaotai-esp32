// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_idf_version.h"
#include "esp_random.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "xiaotai/port/sys.h"

#define CHECK(test) do { if (!(test)) { fprintf(stderr, "line %d: %s\n", __LINE__, #test); return 1; } } while (0)

static const esp_app_desc_t desc = {
    .version = "t1",
    .app_elf_sha256 = {0x00, 0x01, 0x02, 0x03, 0x04, 0x0a, 0x0b, 0x0c,
                       0x80, 0x81, 0x82, 0x83, 0x84, 0x8a, 0x8b, 0x8c,
                       0x90, 0x91, 0x92, 0x93, 0x94, 0x9a, 0x9b, 0x9c,
                       0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xfa, 0xfb, 0xff},
};
static const char full_sha[] = "00010203040a0b0c80818283848a8b8c90919293949a9b9cf0f1f2f3f4fafbff";

const esp_app_desc_t *esp_app_get_description(void) { return &desc; }
const char *esp_get_idf_version(void) { return "v6.0.3"; }
void esp_chip_info(esp_chip_info_t *chip) { *chip = (esp_chip_info_t){.cores = 2, .revision = 2}; }
uint32_t esp_random(void) { return 0x12345678; }
void esp_restart(void) { abort(); }

int esp_app_get_elf_sha256(char *out, size_t size)
{
    if (!out || size < 2) return 0;
    /* The target getter's cached representation is only nine hex digits. */
    const size_t n = size - 1 < CONFIG_APP_RETRIEVE_LEN_ELF_SHA ? size - 1 : CONFIG_APP_RETRIEVE_LEN_ELF_SHA;
    memcpy(out, full_sha, n);
    out[n] = '\0';
    return (int)n + 1;
}

int main(void)
{
    xiaotai_firmware_t info;
    memset(&info, 0xA5, sizeof(info));
    char prefix[sizeof(info.elf_sha256)];
    CHECK(esp_app_get_elf_sha256(prefix, sizeof(prefix)) == 10);
    CHECK(strcmp(prefix, "000102030") == 0);

    xiaotai_firmware(NULL);
    xiaotai_firmware(&info);
    CHECK(memcmp(info.elf_sha256, full_sha, sizeof(full_sha)) == 0);
    CHECK(strcmp(info.version, "t1") == 0);
    CHECK(strcmp(info.platform, "ESP-IDF v6.0.3") == 0);
    CHECK(strcmp(info.chip, "esp32s3") == 0);
    CHECK(info.cores == 2 && info.chip_revision == 2);
    return 0;
}
