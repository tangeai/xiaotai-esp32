# SPDX-License-Identifier: MIT
xiaotai_host_test(part_audio_device SOURCES audio_device_test.c LINK xiaotai_part_audio INCLUDE ../src ../host)
xiaotai_host_test(part_audio_ring SOURCES capture_ring_test.c LINK xiaotai_part_audio INCLUDE ../src ../host)
xiaotai_host_test(part_audio_dma_lifetime SOURCES audio_dma_lifetime_test.c ../src/audio_device.c
                  LINK xiaotai_part_audio INCLUDE ../src ../host)
target_compile_definitions(part_audio_dma_lifetime PRIVATE CONFIG_XIAOTAI_AUDIO_RETAIN_I2S_DMA=1)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME part_audio_native_dma_lifetime
         COMMAND "${CMAKE_COMMAND}" -E env "CC=${CMAKE_C_COMPILER}"
                 "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/native_dma_lifetime_test.py")
set_tests_properties(part_audio_native_dma_lifetime PROPERTIES LABELS xiaotai_part_audio TIMEOUT 120)
add_test(NAME part_audio_aw88298_clock
         COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/native_aw88298_clock_test.py"
                 --cc "${CMAKE_C_COMPILER}")
set_tests_properties(part_audio_aw88298_clock PROPERTIES LABELS xiaotai_part_audio TIMEOUT 120)
if(DEFINED ENV{XIAOTAI_AUDIO_NATIVE_TEST_IDF})
    set(idfs "$ENV{XIAOTAI_AUDIO_NATIVE_TEST_IDF}")
    add_test(NAME part_audio_native_dma_geometry
             COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/native_dma_geometry_test.py" --idf ${idfs})
    set_tests_properties(part_audio_native_dma_geometry PROPERTIES LABELS xiaotai_part_audio TIMEOUT 120)
endif()
