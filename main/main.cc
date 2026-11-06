#include "xiaotai/application/application.h"
#include "esp_log.h"

extern "C" void app_main(void)
{
    static xiaotai::Application application;
    const auto error = application.Start();
    if (error != ESP_OK) {
        ESP_LOGE("XiaoTai", "boot failed phase=%d error=%s",
                 static_cast<int>(application.phase()), esp_err_to_name(error));
#if !XIAOTAI_NATIVE_PROFILE
        ESP_ERROR_CHECK(error);
#endif
    }
}
