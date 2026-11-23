// SPDX-License-Identifier: MIT
/* bus_io PWM on the ESP-IDF LEDC driver (driver/ledc.h), configured as the
 * existing PWM backlights are: low-speed mode (the only one every target
 * has), automatic clock source, no interrupt, hpoint 0. */
#include "bus_io.h"

#include "driver/ledc.h"

xiaotai_err_t xiaotai_bus_io_pwm_setup(const xiaotai_pwm_config_t *config, uint32_t duty)
{
    const ledc_timer_config_t timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = (ledc_timer_t)config->timer,
        .duty_resolution = (ledc_timer_bit_t)config->resolution_bits,
        .freq_hz = config->hz,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    const ledc_channel_config_t channel = {
        .gpio_num = config->pin,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = (ledc_channel_t)config->channel,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = (ledc_timer_t)config->timer,
        .duty = duty,
        .hpoint = 0,
    };
    esp_err_t err = ledc_timer_config(&timer);
    if (err != ESP_OK) return err;
    return ledc_channel_config(&channel);
}

xiaotai_err_t xiaotai_bus_io_pwm_set(uint8_t channel, uint32_t duty)
{
    esp_err_t err = ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel, duty);
    if (err != ESP_OK) return err;
    return ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel);
}

xiaotai_err_t xiaotai_bus_io_pwm_stop(uint8_t channel, bool idle_high)
{
    return ledc_stop(LEDC_LOW_SPEED_MODE, (ledc_channel_t)channel, idle_high);
}
