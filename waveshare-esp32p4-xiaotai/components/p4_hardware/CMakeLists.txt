set(ref "${CMAKE_CURRENT_LIST_DIR}/../../main")
idf_component_register(
 SRCS "p4_video.c" "p4_video_capture.c" "${ref}/hardware/hardware_board.c"
      "${ref}/drivers/camera/camera_driver.c" "${ref}/drivers/display/display_driver.c"
      "${ref}/media/camera_pipeline.c" "${ref}/media/media_governor.c"
      "${ref}/media/media_dma_reserve.c" "${ref}/media/video_yuv420_scaler.c"
      "${ref}/media/video_frame_converter.c" "${ref}/services/call_video_renderer.c"
      "${ref}/platform/app_memory_policy.c"
 INCLUDE_DIRS "." "${ref}/hardware" "${ref}/drivers/display" "${ref}/services"
 PRIV_INCLUDE_DIRS "${ref}/drivers/camera" "${ref}/media" "${ref}/platform"
 REQUIRES esp32_p4_wifi6_touch_lcd_35 lvgl esp_driver_i2c esp_driver_i2s esp_driver_spi
          esp_driver_ledc starter_tirtc
 PRIV_REQUIRES console esp_video esp_h264 esp_timer esp_driver_ppa esp_driver_jpeg esp_codec_dev)
