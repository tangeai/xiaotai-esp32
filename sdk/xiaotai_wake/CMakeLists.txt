# SPDX-License-Identifier: MIT
# XiaoTai wake-word engine (xiaotai/wake.h; manifest name "wake_engine", the
# base module that uses it is "wake"). The portable core (src/) owns the audio
# window, the acceptance rules, the task and the counters; it hears the idle
# device through a tap of the media engine. A model (features, inference,
# scoring) is a registered implementation: the built-in one wraps the
# inference and FFT libraries in esp-idf/, host tests register scripted ones
# and a portable reference front end in host/.
#
# Sources by work package of the wake specification:
#   W1  src/wake.c (task, loading and retries, window, scoring, judging,
#       delivery, listening, threshold, status), src/wake_queue.h,
#       src/wake_window.h, src/internal.h
#   W2  src/model/ (feature front end, quantization, scoring head, tables),
#       host/fft.c
#   W3  esp-idf/ (inference backend, platform FFT, dilated depthwise fast path
#       and its patch script, the embedded model; defines
#       wake_builtin_model_register when XIAOTAI_WAKE_BUILTIN_MODEL is on)
#   W4  src/diagnostics.c (read-only diagnostics provider "wake_engine")
set(sources src/wake.c src/diagnostics.c)
set(model_sources src/model/mel.c src/model/postprocess.c)
if(ESP_PLATFORM)
    set(embedding)
    if(CONFIG_XIAOTAI_WAKE_BUILTIN_MODEL)
        list(APPEND sources ${model_sources} esp-idf/model_backend.cpp esp-idf/fft.c)
        list(APPEND embedding EMBED_FILES esp-idf/model/nihaoxiaotai.tflite)
    endif()
    idf_component_register(SRCS ${sources} ${embedding} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
                           REQUIRES xiaotai_port xiaotai_framework xiaotai_media xiaotai_eventlog xiaotai_at)
    # Scoring stays at -O2 whatever the project's level, as the deployed build
    # compiled it: it changes timing only, never the numbers.
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
    if(CONFIG_XIAOTAI_WAKE_BUILTIN_MODEL AND CONFIG_XIAOTAI_WAKE_DILATED_FAST_PATH)
        include(esp-idf/tflm_depthwise.cmake)
        # IDF registers dependency targets in its component pass; edit the
        # final TFLM source list only after all those targets exist.
        cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL xiaotai_wake_enable_depthwise)
    endif()
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_framework xiaotai_media xiaotai_eventlog xiaotai_at)
add_library(xiaotai_wake STATIC ${sources} ${model_sources} host/fft.c)
target_include_directories(xiaotai_wake PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_wake PUBLIC xiaotai_port xiaotai_framework xiaotai_media xiaotai_eventlog xiaotai_at
                     PRIVATE m)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
