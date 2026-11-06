idf_component_register(
    SRCS
        "${CMAKE_CURRENT_LIST_DIR}/src/starter_voice.c"
        "${CMAKE_CURRENT_LIST_DIR}/src/wake_backend.cpp"
        "${CMAKE_CURRENT_LIST_DIR}/vendor/mel_extractor.c"
    INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/include"
    PRIV_INCLUDE_DIRS "${CMAKE_CURRENT_LIST_DIR}/vendor" "${CMAKE_CURRENT_LIST_DIR}/model" "${CMAKE_CURRENT_LIST_DIR}/src"
    REQUIRES esp_common
    PRIV_REQUIRES esp_timer esp-tflite-micro esp-dsp
)
target_add_binary_data(${COMPONENT_LIB} "${CMAKE_CURRENT_LIST_DIR}/model/nihaoxiaotai.tflite" BINARY RENAME_TO "nihaoxiaotai.tflite")
