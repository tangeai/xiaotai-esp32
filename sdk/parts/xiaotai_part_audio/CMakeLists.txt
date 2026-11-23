# SPDX-License-Identifier: MIT
# XiaoTai parts library: PCM audio on an I2S port (xiaotai/parts/audio.h) -
# one audio device with its codec chips (ES8311, ES8389, ES7210, AW88298) gives the
# board an audio input and an audio output part.
# Use stock esp_codec_dev for the codec-chip interfaces, not the vendor I2S
# adapter. audio_i2s.c owns channel state and makes enable/disable idempotent.
set(sources src/audio_device.c src/capture_ring.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} "esp-idf/audio_i2s.c" "esp-idf/codec_native.c"
                           "esp-idf/es8311.c" "esp-idf/es8389.c" "esp-idf/es7210.c" "esp-idf/aw88298.c"
                           INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
                           REQUIRES xiaotai_port xiaotai_hal xiaotai_part_bus
                           PRIV_REQUIRES esp_driver_i2s esp_driver_gpio esp_driver_i2c hal)
    idf_component_optional_requires(PRIVATE esp_codec_dev espressif__esp_codec_dev)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_hal xiaotai_part_bus)
add_library(xiaotai_part_audio STATIC ${sources} host/audio_io.c host/codecs.c)
target_include_directories(xiaotai_part_audio PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_part_audio PUBLIC xiaotai_port xiaotai_hal xiaotai_part_bus)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
