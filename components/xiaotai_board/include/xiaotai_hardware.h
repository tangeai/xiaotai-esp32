#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The selected board's identity as static strings: model and hardware revision. */
const char *xiaotai_board_model(void);
const char *xiaotai_board_revision(void);

/* Static board model token (ASCII letters, digits, '.', '_' or '-'), or NULL
 * when the board has not defined a device profile model. */
const char *xiaotai_board_profile_model(void);

/* Configure the user button. Afterwards one task polls
 * xiaotai_board_user_button_clicked() every 10 ms; it returns true once per
 * completed press. The board owns debounce and the hardware (GPIO level, PMIC
 * key event); GPIO boards can use xiaotai_level_button.h. */
#define XIAOTAI_BOARD_BUTTON_POLL_MS 10U
esp_err_t xiaotai_board_user_button_open(void);
bool xiaotai_board_user_button_clicked(void);
#ifdef __cplusplus
}
#endif
