#include "device_video_profile.h"

#include <stdio.h>
#include <string.h>

static void device_video_copy_text(char *dst, size_t dst_size,
                                   const char *src) {
  if (dst == NULL || dst_size == 0) {
    return;
  }
  if (src == NULL) {
    dst[0] = '\0';
    return;
  }
  snprintf(dst, dst_size, "%s", src);
}

const char *device_video_input_mode_name(device_video_input_mode_t mode) {
  if (mode == DEVICE_VIDEO_INPUT_CAMERA) {
    return "camera";
  }
  return "file";
}

void device_video_config_init_defaults(device_video_config_t *config) {
  if (config == NULL) {
    return;
  }

  memset(config, 0, sizeof(*config));
  config->input_mode = DEVICE_VIDEO_INPUT_FILE;
  device_video_copy_text(config->input_path, sizeof(config->input_path),
                         DEVICE_VIDEO_DEFAULT_INPUT);
  config->fps = DEVICE_VIDEO_DEFAULT_FPS;
  config->frame_interval_us = 0;
  config->bitrate_kbps = DEVICE_VIDEO_DEFAULT_BITRATE_KBPS;
  config->gop = DEVICE_VIDEO_DEFAULT_GOP;
  config->log_level = DEVICE_VIDEO_DEFAULT_LOG_LEVEL;
  config->loop = false;

  device_video_copy_text(config->group, sizeof(config->group),
                         DEVICE_VIDEO_DEFAULT_GROUP);
  device_video_copy_text(config->device_id, sizeof(config->device_id),
                         DEVICE_VIDEO_DEFAULT_DEVICE_ID);
  config->link_param = 0;
  config->encrypt_level = 0;
  config->shared_thread = 0;
  config->return_host = 0;

  device_video_copy_text(config->ws_host, sizeof(config->ws_host),
                         DEVICE_VIDEO_DEFAULT_WS_HOST);
  config->ws_port = DEVICE_VIDEO_DEFAULT_WS_PORT;
  device_video_copy_text(config->ws_path, sizeof(config->ws_path),
                         DEVICE_VIDEO_DEFAULT_WS_PATH);
}
