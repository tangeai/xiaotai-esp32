set(shared "${CMAKE_CURRENT_LIST_DIR}/../../../lckfb-szpi-esp32s3-tirtc/components/starter_voice")
idf_component_register(
    SRCS
        "${shared}/src/starter_voice.c"
        "${shared}/src/wake_backend.cpp"
        "${shared}/vendor/mel_extractor.c"
    INCLUDE_DIRS "${shared}/include"
    PRIV_INCLUDE_DIRS "${shared}/vendor" "${shared}/model" "${shared}/src"
    REQUIRES esp_common
    PRIV_REQUIRES esp_timer esp-tflite-micro esp-dsp
)
target_add_binary_data(${COMPONENT_LIB} "${shared}/model/nihaoxiaotai.tflite" BINARY RENAME_TO "nihaoxiaotai.tflite")
