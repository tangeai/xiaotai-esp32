#pragma once
#include "esp_err.h"

namespace xiaotai {

enum class BootPhase { Cold, Platform, Media, View, Network, Services, Started };

class Application final {
public:
    // Started means local initialization and worker handoff, not cloud readiness.
    esp_err_t Start();
    BootPhase phase() const { return phase_; }
    esp_err_t error() const { return error_; }

private:
    BootPhase phase_ = BootPhase::Cold;
    esp_err_t error_ = ESP_OK;
};

}  // namespace xiaotai
