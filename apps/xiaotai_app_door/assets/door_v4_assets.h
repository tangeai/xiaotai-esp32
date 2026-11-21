/* Retained V4 assets; see README.md. */
#pragma once
#include "lvgl.h"
#if LV_COLOR_DEPTH != 16 || LVGL_VERSION_MAJOR != 9
#error "Door assets require native little-endian RGB565"
#endif
LV_IMAGE_DECLARE(door_asset_back_28);
LV_IMAGE_DECLARE(door_asset_back_36);
LV_IMAGE_DECLARE(door_asset_call_online_8);
LV_IMAGE_DECLARE(door_asset_circle_action_72);
LV_IMAGE_DECLARE(door_asset_circle_incoming_action_72);
LV_IMAGE_DECLARE(door_asset_circle_incoming_red_72);
LV_IMAGE_DECLARE(door_asset_circle_monitor_action_72);
LV_IMAGE_DECLARE(door_asset_circle_monitor_neutral_72);
LV_IMAGE_DECLARE(door_asset_circle_neutral_72);
LV_IMAGE_DECLARE(door_asset_circle_red_72);
LV_IMAGE_DECLARE(door_asset_close_28);
LV_IMAGE_DECLARE(door_asset_failure_52);
LV_IMAGE_DECLARE(door_asset_hangup_36);
LV_IMAGE_DECLARE(door_asset_incoming_8);
LV_IMAGE_DECLARE(door_asset_key_28);
LV_IMAGE_DECLARE(door_asset_key_36);
LV_IMAGE_DECLARE(door_asset_key_call_36);
LV_IMAGE_DECLARE(door_asset_key_monitor_36);
LV_IMAGE_DECLARE(door_asset_knob_20);
LV_IMAGE_DECLARE(door_asset_lock_32);
LV_IMAGE_DECLARE(door_asset_mic_36);
LV_IMAGE_DECLARE(door_asset_online_8);
LV_IMAGE_DECLARE(door_asset_phone_36);
LV_IMAGE_DECLARE(door_asset_video_36);
LV_IMAGE_DECLARE(door_asset_volume_36);
LV_IMAGE_DECLARE(door_asset_wifi_24);
