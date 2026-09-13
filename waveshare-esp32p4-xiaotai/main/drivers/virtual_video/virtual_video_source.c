#include "virtual_video_source.h"

#include <string.h>

void virtual_video_source_prepare_config(device_video_config_t *config)
{
    if (config == NULL) {
        return;
    }

    device_video_config_init_defaults(config);
    strlcpy(config->input_path, VIRTUAL_VIDEO_SOURCE_DEFAULT_PATH, sizeof(config->input_path));
    config->loop = true;

    if (config->frame_interval_us == 0U) {
        config->frame_interval_us = 1000000U / DEVICE_VIDEO_DEFAULT_FPS;
    }
    if (config->fps <= 0) {
        config->fps = DEVICE_VIDEO_DEFAULT_FPS;
    }
}
