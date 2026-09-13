set(shared "${CMAKE_CURRENT_LIST_DIR}/../../../lckfb-szpi-esp32s3-tirtc/components/starter_media")
idf_component_register(
 SRCS "src/starter_media.c" "src/starter_aec.c" "src/p4_capture_highpass.c"
      "src/p4_audio_playout.c" "src/audio_playout_controller.c"
      "${shared}/src/starter_audio_resampler.c" "${shared}/src/starter_preroll.c"
      "${shared}/src/starter_agc.c"
 INCLUDE_DIRS "${shared}/include"
 PRIV_INCLUDE_DIRS "${shared}/src"
 REQUIRES starter_tirtc starter_voice esp_driver_i2c p4_hardware
 PRIV_REQUIRES esp_driver_gpio esp_driver_i2s esp_timer freertos log esp_codec_dev esp_audio_codec esp-sr)
