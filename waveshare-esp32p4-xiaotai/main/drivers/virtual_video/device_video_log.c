#include "device_video_profile.h"

#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

uint64_t device_video_now_ms(void) {
  struct timeval tv;

  gettimeofday(&tv, NULL);
  return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

void device_video_sleep_ms(int ms) {
  TickType_t ticks = 0;

  if (ms <= 0) {
    return;
  }

  if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED) {
    usleep((useconds_t)ms * 1000U);
    return;
  }

  ticks = pdMS_TO_TICKS((uint32_t)ms);
  if (ticks == 0) {
    ticks = 1;
  }
  vTaskDelay(ticks);
}

void device_video_stats_init(device_video_sender_stats_t *stats) {
  if (stats == NULL) {
    return;
  }

  memset(stats, 0, sizeof(*stats));
  stats->started_at_ms = device_video_now_ms();
}

void device_video_stats_mark_error(device_video_sender_stats_t *stats, int code,
                                   const char *stage) {
  if (stats == NULL) {
    return;
  }

  stats->last_error_code = code;
  if (stage == NULL) {
    stats->last_error_stage[0] = '\0';
    return;
  }

  snprintf(stats->last_error_stage, sizeof(stats->last_error_stage), "%s",
           stage);
}
