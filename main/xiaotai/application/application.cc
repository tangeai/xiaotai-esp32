#include "application.h"
#include "board.h"
#if !XIAOTAI_NATIVE_PROFILE
#include "startup_services.h"
#include "starter_media.h"
#include "starter_product.h"
#include "wifi_manager.h"
#else
extern "C" esp_err_t xiaotai_profile_main(void);
#endif
#include "esp_log.h"

namespace xiaotai {

esp_err_t Application::Start()
{
    if (phase_ == BootPhase::Started) return ESP_OK;
    if (phase_ != BootPhase::Cold) return error_ == ESP_OK ? ESP_ERR_INVALID_STATE : error_;

    const auto identity = board::SelectedBoard().identity();
    ESP_LOGI("XiaoTai", "starting %s revision=%s", identity.model, identity.revision);

#if XIAOTAI_NATIVE_PROFILE
    // Existing profile owns its event loop and failure semantics. No second
    // S3 task, PSRAM stack or parallel RTC owner is started for this profile.
    phase_ = BootPhase::Services;
    error_ = xiaotai_profile_main();
    if (error_ != ESP_OK) return error_;
    phase_ = BootPhase::Started;
    return ESP_OK;
#else

    phase_ = BootPhase::Platform;
    error_ = xiaotai_startup_initialize_platform();
    if (error_ != ESP_OK) return error_;

    phase_ = BootPhase::Media;
    error_ = starter_media_init();
    if (error_ != ESP_OK) return error_;
    xiaotai_startup_log_heap("post-media");

    phase_ = BootPhase::View;
    error_ = starter_product_start();
    if (error_ != ESP_OK) return error_;
    xiaotai_startup_log_heap("post-product-ui");

    phase_ = BootPhase::Network;
    error_ = wifi_manager_start();
    if (error_ != ESP_OK) return error_;
    xiaotai_startup_log_heap("post-wifi-start");

    // The worker owns binding and online services. Wake model loading remains
    // after RTC bootstrap, avoiding simultaneous peaks in internal memory.
    phase_ = BootPhase::Services;
    error_ = xiaotai_startup_launch_services();
    if (error_ != ESP_OK) return error_;
    phase_ = BootPhase::Started;
    return ESP_OK;
#endif
}

}  // namespace xiaotai
