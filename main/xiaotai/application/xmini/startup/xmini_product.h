// SPDX-License-Identifier: MIT
#pragma once
#include "esp_err.h"
#include "xmini_guide.h"
#include "xmini_ui.h"

esp_err_t xmini_product_init(esp_err_t runtime_error);
void xmini_product_poll(xmini_ui_t *ui);
esp_err_t xmini_product_confirm(void);
// -1 cancels only an unplayed business feedback, never a binding/network prompt.
// Cancel obsolete session feedback before poll; the caller owns its phase/generation.
esp_err_t xmini_product_feedback(int prompt);
xmini_guide_t xmini_product_guide(void);
