idf_component_register(
    SRCS
        "src/starter_aec.c"
        "src/starter_agc.c"
        "src/starter_audio_resampler.c"
        "src/starter_preroll.c"
        "src/starter_playout.c"
        "src/starter_media.c"
    INCLUDE_DIRS "include"
    REQUIRES starter_tirtc starter_voice esp_driver_i2c
    PRIV_REQUIRES
        esp_driver_gpio
        esp_driver_i2s
        esp_timer
        freertos
        log
        esp_codec_dev
        esp_audio_codec
        esp-sr
)

# These sample-by-sample loops share the 32 ms dual-AEC deadline. Keep debug
# symbols, but avoid the project's -Og loop overhead here. No fast-math or
# global optimization change: codec/AEC, networking and UI keep their flags.
set_source_files_properties(
    "src/starter_aec.c"
    "src/starter_audio_resampler.c"
    PROPERTIES COMPILE_OPTIONS "-O2"
)
