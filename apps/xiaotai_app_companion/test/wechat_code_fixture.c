// SPDX-License-Identifier: MIT
#include "xiaotai/ui.h"

/* Monochrome pixels only: this pattern does not encode a QR or other payload. */
static const uint8_t pixels[] = {0xaa, 0x80, 0x55, 0x00};
const xiaotai_ui_image_t xiaotai_companion_wechat_code = {
    .name = "companion.wechat_code", .width = 9, .height = 2,
    .format = XIAOTAI_IMAGE_MONO1, .data = pixels, .size = sizeof(pixels),
};
