// SPDX-License-Identifier: MIT
#include "harness.h"
#include "xiaotai/port/kv.h"

int main(void)
{
    fakes_reset();
    CHECK(harness_start(NULL, true) == 0);
    CHECK(owner_status().enabled);
    int32_t value = 0;
    CHECK(xiaotai_setting_get_int("ai_vision.enabled", &value) == XIAOTAI_ERR_NOT_FOUND);
    CHECK(post_vision_intent(0) == XIAOTAI_OK && !owner_status().enabled);
    CHECK(xiaotai_kv_flush(3000) == XIAOTAI_OK);
    size_t size = sizeof(value);
    CHECK(xiaotai_kv_get("xt_settings", "ai_vision", &value, &size, 1000) == XIAOTAI_ERR_NOT_FOUND);
    harness_stop();

    CHECK(harness_start(NULL, false) == 0);
    CHECK(owner_status().enabled);
    harness_stop();

    const xiaotai_ai_vision_config_t config = {.user_switch = true};
    CHECK(harness_start(&config, false) == 0);
    CHECK(owner_status().enabled);
    CHECK(owner_setting_set("ai_vision.enabled", 0) == XIAOTAI_OK);
    CHECK(!owner_status().enabled);
    CHECK(xiaotai_kv_flush(3000) == XIAOTAI_OK);
    harness_stop();

    CHECK(harness_start(&config, false) == 0);
    CHECK(!owner_status().enabled);
    CHECK(xiaotai_setting_get_int("ai_vision.enabled", &value) == XIAOTAI_OK && value == 0);
    owner_quiesce(true);
    CHECK(owner_set_enabled(true) == XIAOTAI_ERR_BUSY);
    CHECK(!owner_status().enabled);
    owner_quiesce(false);
    CHECK(owner_set_enabled(true) == XIAOTAI_OK && owner_status().enabled);
    CHECK(xiaotai_kv_flush(3000) == XIAOTAI_OK);
    harness_stop();

    CHECK(harness_start(&config, false) == 0);
    CHECK(owner_status().enabled);
    harness_stop();
    return 0;
}
