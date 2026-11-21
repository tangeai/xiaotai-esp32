// SPDX-License-Identifier: MIT
/* bus_io pins on the ESP-IDF GPIO driver (driver/gpio.h). */
#include "bus_io.h"

#include "driver/gpio.h"

xiaotai_err_t xiaotai_bus_io_pin_setup(const xiaotai_gpio_config_t *config, bool output, bool level)
{
    /* Outputs keep their input buffer (INPUT_OUTPUT) so a line reads back the
     * level the pin is at; enabling the buffer does not change what the pin
     * drives. */
    gpio_mode_t mode = GPIO_MODE_INPUT;
    if (output) mode = config->open_drain ? GPIO_MODE_INPUT_OUTPUT_OD : GPIO_MODE_INPUT_OUTPUT;
    const gpio_config_t pin = {
        .pin_bit_mask = 1ULL << config->pin,
        .mode = mode,
        .pull_up_en = config->pull_up ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE,
        .pull_down_en = config->pull_down ? GPIO_PULLDOWN_ENABLE : GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    if (output) {
        /* Latch the level before the pin starts driving (an amplifier enable
         * must not pulse on at boot), and again after: configuring an output
         * leaves the output register as it was, the second write makes that
         * independent of the driver version. */
        esp_err_t err = gpio_set_level(config->pin, level);
        if (err != ESP_OK) return err;
        err = gpio_config(&pin);
        if (err != ESP_OK) return err;
        return gpio_set_level(config->pin, level);
    }
    return gpio_config(&pin);
}

xiaotai_err_t xiaotai_bus_io_pin_set(int pin, bool level)
{
    return gpio_set_level(pin, level ? 1U : 0U);
}

xiaotai_err_t xiaotai_bus_io_pin_get(int pin, bool *level)
{
    if (!GPIO_IS_VALID_GPIO(pin)) return XIAOTAI_ERR_INVALID_ARG;
    *level = gpio_get_level(pin) != 0;
    return XIAOTAI_OK;
}
