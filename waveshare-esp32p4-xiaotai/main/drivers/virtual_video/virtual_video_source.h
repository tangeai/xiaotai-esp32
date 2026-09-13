#pragma once

#include "device_video_profile.h"

#define VIRTUAL_VIDEO_SOURCE_DEFAULT_PATH "/spiffs/send_video.h264"

void virtual_video_source_prepare_config(device_video_config_t *config);
