idf_component_register(
    SRCS
        "src/starter_voice.c"
        "src/wake_backend.cpp"
        "vendor/mel_extractor.c"
    INCLUDE_DIRS "include"
    PRIV_INCLUDE_DIRS "vendor" "model" "src"
    REQUIRES esp_common
    PRIV_REQUIRES esp_timer esp-tflite-micro esp-dsp
    EMBED_FILES "model/nihaoxiaotai.tflite"
)

# Keep the exact model/frontend arithmetic (no fast-math), but do not execute
# the 98-frame feature extractor at the project's debug-only -Og setting.
target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
