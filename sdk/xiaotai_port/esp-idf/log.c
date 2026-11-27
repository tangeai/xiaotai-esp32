// SPDX-License-Identifier: MIT
/* xiaotai/port/log.h on ESP-IDF: the system threshold and the start-up
 * defaults. The app threshold and the module thresholds are common/log_gate.c. */
#include "xiaotai/port/log.h"

#include <stdatomic.h>
#include "esp_log.h"
#include "sdkconfig.h"

/* Every XiaoTai line is written under this ESP-IDF tag (xiaotai_log_port.h);
 * it is held at error so the line passes whatever "*" is set to. */
const char xiaotai_log_gate_tag[] = "xiaotai.log";

/* Third-party tags that warn about what is normal for this firmware. While
 * the system threshold is warning or below they are held at error; raising
 * the threshold to info shows everything they print.
 *   wifi         the driver lists its rate table and every block-ack session
 *                at warning level; link changes are reported by the network
 *                engine in its own words
 *   AFE_CONFIG   "wakenet model not found": the wake word runs in XiaoTai's
 *                own engine, never inside the audio front end
 *   i2s_common   "rx channel switched from master to slave": how full-duplex
 *                I2S is meant to be opened */
static const char *const s_benign[] = {"wifi", "AFE_CONFIG", "i2s_common"};

#define SYSTEM_MAX ((unsigned)CONFIG_LOG_MAXIMUM_LEVEL)
static atomic_uint s_system = CONFIG_LOG_DEFAULT_LEVEL;

static void apply_system(unsigned level)
{
    /* "*" forgets every per-tag level, so the fixed ones follow it. */
    esp_log_level_set("*", (esp_log_level_t)level);
    esp_log_level_set(xiaotai_log_gate_tag, ESP_LOG_ERROR);
    if (level > ESP_LOG_WARN) return;
    for (size_t i = 0; i < sizeof(s_benign) / sizeof(s_benign[0]); ++i) {
        esp_log_level_set(s_benign[i], level < ESP_LOG_ERROR ? (esp_log_level_t)level : ESP_LOG_ERROR);
    }
}

xiaotai_err_t xiaotai_log_set_system_level(uint8_t level)
{
    if (level > SYSTEM_MAX) return XIAOTAI_ERR_INVALID_ARG;
    apply_system(level);
    atomic_store(&s_system, level);
    return XIAOTAI_OK;
}

uint8_t xiaotai_log_system_level(void) { return (uint8_t)atomic_load(&s_system); }
uint8_t xiaotai_log_system_level_max(void) { return (uint8_t)SYSTEM_MAX; }

void xiaotai_log_start(void) { apply_system(atomic_load(&s_system)); }

void xiaotai_log_quiet_for_restart(void)
{
    /* Whoever raised a threshold wants to see the teardown too; at the
     * defaults only errors remain, XiaoTai's own. */
    if (xiaotai_log_level() == XIAOTAI_LOG_LEVEL_WARN || xiaotai_log_level() == XIAOTAI_LOG_LEVEL_INFO) {
        (void)xiaotai_log_set_level(XIAOTAI_LOG_LEVEL_ERROR);
    }
    if (atomic_load(&s_system) > ESP_LOG_WARN) return;
    esp_log_level_set("*", ESP_LOG_NONE);
    esp_log_level_set(xiaotai_log_gate_tag, ESP_LOG_ERROR);
}
