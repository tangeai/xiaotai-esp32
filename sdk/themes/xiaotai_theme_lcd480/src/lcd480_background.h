// SPDX-License-Identifier: MIT
#pragma once

#include "lvgl.h"

/* Exact RGB565 values; the edge is the background of every other page. */
#define LCD480_BACKGROUND_CENTER 0x181c21U
#define LCD480_BACKGROUND_EDGE 0x101418U

/* The caller keeps the zero-initialized descriptor and owns its data. */
bool lcd480_background(lv_image_dsc_t *image, uint16_t width, uint16_t height);
