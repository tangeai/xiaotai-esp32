idf_component_register(
 SRCS "src/starter_media.c" "src/starter_aec.c" "src/p4_capture_highpass.c"
      "src/p4_audio_playout.c" "src/audio_playout_controller.c" "src/p4_playback_resampler.c"
      "${CMAKE_CURRENT_LIST_DIR}/src/starter_preroll.c"
      "${CMAKE_CURRENT_LIST_DIR}/src/starter_agc.c"
      "${CMAKE_CURRENT_LIST_DIR}/src/audio_tx_timing.c"
 INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/include"
 PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/src" "${CMAKE_CURRENT_LIST_DIR}/../p4_hardware/native/platform"
 REQUIRES starter_tirtc starter_voice esp_driver_i2c p4_hardware
 PRIV_REQUIRES esp_driver_gpio esp_driver_i2s esp_timer freertos log lwip esp_codec_dev esp_audio_codec esp-sr esp-dsp)
