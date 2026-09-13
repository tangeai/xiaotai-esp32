#include "app.h"

#include "esp_err.h"
#include "app_log_policy.h"

void app_main(void)
{
    app_log_policy_apply();
    ESP_ERROR_CHECK(app_init());
    app_run();
}
