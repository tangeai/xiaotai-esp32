idf_component_register(SRCS "xmini_audio.c" "xmini_echo_guard.c" INCLUDE_DIRS "include"
                       REQUIRES xmini_board esp_driver_i2s esp_driver_gpio
                                esp_codec_dev esp_audio_codec esp_timer
                       EMBED_FILES "assets/guidance.alaw")
