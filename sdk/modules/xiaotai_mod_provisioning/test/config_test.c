// SPDX-License-Identifier: MIT
/* mod_provisioning_config: the build options in Kconfig and
 * xiaotai/modules/provisioning_config.h agree (every option in both, the same
 * default), and the defaults are the behaviour the public header states: a
 * shared bound of 5 s, a failed begin released, no hold after a setting
 * failure, no packed write barrier, no automatic store retry. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xiaotai/modules/provisioning_config.h"

#define CHECK(condition)                                                                  \
    do {                                                                                  \
        if (!(condition)) {                                                               \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
            return 1;                                                                     \
        }                                                                                 \
    } while (0)

static char s_kconfig[8192];

/* The default line of "config <name>" as an integer (y/n as 1/0); -1 when absent. */
static long kconfig_default(const char *name)
{
    char key[96];
    snprintf(key, sizeof(key), "config %s\n", name);
    const char *at = strstr(s_kconfig, key);
    if (!at) return -1;
    const char *next = strstr(at + strlen(key), "config ");
    const char *value = strstr(at, "default ");
    if (!value || (next && value > next)) return -1;
    value += strlen("default ");
    if (*value == 'y') return 1;
    if (*value == 'n') return 0;
    return strtol(value, NULL, 0);
}

int main(void)
{
    FILE *file = fopen(XIAOTAI_TEST_KCONFIG, "r");
    CHECK(file != NULL);
    const size_t length = fread(s_kconfig, 1, sizeof(s_kconfig) - 1, file);
    fclose(file);
    s_kconfig[length] = '\0';

    CHECK(kconfig_default("XIAOTAI_PROVISIONING_LOCAL_POLICY") == XIAOTAI_PROVISIONING_LOCAL_POLICY);
    CHECK(kconfig_default("XIAOTAI_PROVISIONING_SETTLE_WAIT_MS") == XIAOTAI_PROVISIONING_SETTLE_WAIT_MS);
    CHECK(kconfig_default("XIAOTAI_PROVISIONING_BEGIN_FAILURE") == XIAOTAI_PROVISIONING_BEGIN_FAILURE);
    CHECK(kconfig_default("XIAOTAI_PROVISIONING_STORE_SETTINGS_HOLD") == XIAOTAI_PROVISIONING_STORE_SETTINGS_HOLD);
    CHECK(kconfig_default("XIAOTAI_PROVISIONING_STORE_PACKED_RETRY") == XIAOTAI_PROVISIONING_STORE_PACKED_RETRY);
    CHECK(kconfig_default("XIAOTAI_PROVISIONING_STORE_RETRY_MS") == XIAOTAI_PROVISIONING_STORE_RETRY_MS);
    /* Every option of the Kconfig is checked above. */
    int options = 0;
    for (const char *at = strstr(s_kconfig, "\n    config "); at; at = strstr(at + 1, "\n    config ")) ++options;
    CHECK(options == 6);

    CHECK(XIAOTAI_PROVISIONING_LOCAL_POLICY == XIAOTAI_PROVISIONING_LOCAL_ORDERLY);
    CHECK(XIAOTAI_PROVISIONING_SETTLE_WAIT_MS == 5000);
    CHECK(XIAOTAI_PROVISIONING_BEGIN_FAILURE == XIAOTAI_PROVISIONING_BEGIN_FAILURE_RELEASE);
    CHECK(XIAOTAI_PROVISIONING_STORE_SETTINGS_HOLD == 0);
    CHECK(XIAOTAI_PROVISIONING_STORE_PACKED_RETRY == 0);
    CHECK(XIAOTAI_PROVISIONING_STORE_RETRY_MS == 0);
    puts("mod_provisioning_config: passed");
    return 0;
}
