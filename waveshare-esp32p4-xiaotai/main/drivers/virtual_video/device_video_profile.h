#ifndef DEVICE_VIDEO_PROFILE_H_
#define DEVICE_VIDEO_PROFILE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#define DEVICE_VIDEO_TEXT_LEN 256
#define DEVICE_VIDEO_STAGE_LEN 64

#define DEVICE_VIDEO_OK 0
#define DEVICE_VIDEO_ERR_INVALID_ARG -1
#define DEVICE_VIDEO_ERR_IO -2
#define DEVICE_VIDEO_ERR_TIMEOUT -4
#define DEVICE_VIDEO_ERR_EOF -5

#define DEVICE_VIDEO_DEFAULT_INPUT "/spiffs/send_video.h264"
#define DEVICE_VIDEO_DEFAULT_FPS 25
#define DEVICE_VIDEO_DEFAULT_BITRATE_KBPS 1500
#define DEVICE_VIDEO_DEFAULT_GOP 25
#define DEVICE_VIDEO_DEFAULT_LOG_LEVEL 1
#define DEVICE_VIDEO_DEFAULT_GROUP "TEST"
#define DEVICE_VIDEO_DEFAULT_DEVICE_ID "DEVICE-VIDEO-001"
#define DEVICE_VIDEO_DEFAULT_WS_HOST "0.0.0.0"
#define DEVICE_VIDEO_DEFAULT_WS_PORT 8765
#define DEVICE_VIDEO_DEFAULT_WS_PATH "/"

typedef enum {
  DEVICE_VIDEO_INPUT_FILE = 0,
  DEVICE_VIDEO_INPUT_CAMERA = 1,
} device_video_input_mode_t;

typedef struct {
  device_video_input_mode_t input_mode;
  char input_path[DEVICE_VIDEO_TEXT_LEN];
  int fps;
  uint32_t frame_interval_us;
  int bitrate_kbps;
  int gop;
  int log_level;
  bool loop;

  char group[DEVICE_VIDEO_TEXT_LEN];
  char device_id[DEVICE_VIDEO_TEXT_LEN];
  int link_param;
  int encrypt_level;
  int shared_thread;
  int return_host;

  char ws_host[DEVICE_VIDEO_TEXT_LEN];
  int ws_port;
  char ws_path[DEVICE_VIDEO_TEXT_LEN];
} device_video_config_t;

typedef struct {
  FILE *fp;
  uint8_t *data;
  size_t size;
  size_t capacity;
  size_t consumed_bytes;
  bool file_end_reached;
} device_video_h264_file_t;

typedef struct {
  uint64_t started_at_ms;
  uint64_t ended_at_ms;
  uint64_t bytes_sent;
  uint64_t frames_sent;
  uint64_t send_failures;
  int last_error_code;
  char last_error_stage[DEVICE_VIDEO_STAGE_LEN];
} device_video_sender_stats_t;

const char *device_video_input_mode_name(device_video_input_mode_t mode);

void device_video_config_init_defaults(device_video_config_t *config);

long device_video_source_file_size(const char *path);
int device_video_source_file_validate(const device_video_config_t *config);
int device_video_source_file_open(const char *path,
                                  device_video_h264_file_t *file);
void device_video_source_file_reset(device_video_h264_file_t *file);
void device_video_source_file_close(device_video_h264_file_t *file);
int device_video_source_file_next_frame(device_video_h264_file_t *file,
                                        const uint8_t **data_ptr,
                                        size_t *data_len, int *is_key_frame);
int device_video_source_camera_prepare(const device_video_config_t *config);

uint64_t device_video_now_ms(void);
void device_video_sleep_ms(int ms);
void device_video_stats_init(device_video_sender_stats_t *stats);
void device_video_stats_mark_error(device_video_sender_stats_t *stats, int code,
                                   const char *stage);

#endif
