cmake_minimum_required(VERSION 3.16)
project(xmini_ui_tests C)
set(CMAKE_C_STANDARD 11)
option(XMINI_HOST_SANITIZERS "Run all C host tests with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
if(XMINI_HOST_SANITIZERS)
    if(MSVC)
        message(FATAL_ERROR "The combined ASAN/UBSAN suite requires GCC or Clang; run it in WSL")
    endif()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
endif()
enable_testing()
set(ESP_IDF_PATH "$ENV{IDF_PATH}" CACHE PATH "ESP-IDF source for upstream protocol test dependencies")
if(NOT ESP_IDF_PATH AND EXISTS "/mnt/c/esp/v5.5.5/esp-idf")
    set(ESP_IDF_PATH "/mnt/c/esp/v5.5.5/esp-idf")
endif()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME xmini_guidance_assets COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_guidance_assets.py)
add_test(NAME xmini_regress_product_selftest COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/../tools/regress_product.py --self-test)
add_test(NAME xmini_regression_settings COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/test_regression_settings.py)
if(NOT MSVC)
    foreach(source components/json/cJSON/cJSON.c)
        if(NOT EXISTS "${ESP_IDF_PATH}/${source}")
            message(FATAL_ERROR "Full host tests require ESP-IDF source: ${source}; set ESP_IDF_PATH explicitly")
        endif()
    endforeach()
    add_executable(xmini_usb_monitor_tests test_usb_monitor.c)
    target_include_directories(xmini_usb_monitor_tests PRIVATE usb_stubs)
    target_compile_options(xmini_usb_monitor_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME xmini_usb_monitor COMMAND xmini_usb_monitor_tests)
    add_executable(xmini_audio_stream_tests test_audio_stream.c ../components/xmini_audio/xmini_echo_guard.c)
    target_include_directories(xmini_audio_stream_tests PRIVATE audio_stubs ../components/xmini_audio/include)
    target_compile_options(xmini_audio_stream_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME xmini_audio_stream COMMAND xmini_audio_stream_tests)
    add_executable(xmini_echo_guard_tests test_echo_guard.c ../components/xmini_audio/xmini_echo_guard.c)
    target_include_directories(xmini_echo_guard_tests PRIVATE ../components/xmini_audio/include)
    target_compile_options(xmini_echo_guard_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_link_libraries(xmini_echo_guard_tests PRIVATE m)
    add_test(NAME xmini_echo_guard COMMAND xmini_echo_guard_tests)
    add_executable(xmini_feedback_tests test_feedback.c)
    target_include_directories(xmini_feedback_tests PRIVATE ../main
        ../components/xmini_business/tests/stubs ../components/xmini_rtc/tests/stubs
        ../components/xmini_business/include ../components/xmini_group/include
        ../components/xmini_audio/include ../components/xmini_rtc/include
        ../components/tirtc_sdk/include ../components/demo_platform/include)
    target_compile_options(xmini_feedback_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME xmini_feedback COMMAND xmini_feedback_tests)
    set(XMINI_APP_MEMORY_ARGS --compiler "${CMAKE_C_COMPILER}")
    if(XMINI_HOST_SANITIZERS)
        list(APPEND XMINI_APP_MEMORY_ARGS --sanitize)
    endif()
    add_test(NAME xmini_app_memory COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_app_memory.py ${XMINI_APP_MEMORY_ARGS})
    add_executable(xmini_rtc_tests ../components/xmini_rtc/tests/test_rtc.c ../components/xmini_rtc/xmini_rtc.c)
    target_include_directories(xmini_rtc_tests PRIVATE ../components/xmini_rtc/tests/stubs
        ../components/xmini_rtc/include ../components/tirtc_sdk/include)
    target_compile_options(xmini_rtc_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME xmini_rtc COMMAND xmini_rtc_tests)
    add_executable(xmini_rx_repacketize_tests ../components/xmini_rtc/tests/test_rx_repacketize.c
        ../components/xmini_rtc/xmini_rtc.c)
    target_include_directories(xmini_rx_repacketize_tests PRIVATE ../components/xmini_rtc/tests/stubs
        ../components/xmini_rtc/include ../components/tirtc_sdk/include)
    target_compile_options(xmini_rx_repacketize_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_link_options(xmini_rx_repacketize_tests PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
    add_test(NAME xmini_rx_repacketize COMMAND xmini_rx_repacketize_tests)
    add_executable(xmini_business_tests ../components/xmini_business/tests/test_business.c
        ../components/xmini_business/xmini_business.c "${ESP_IDF_PATH}/components/json/cJSON/cJSON.c")
    target_include_directories(xmini_business_tests PRIVATE ../components/xmini_business/tests/stubs
        ../components/xmini_rtc/tests/stubs ../components/xmini_business/include ../components/xmini_group/include
        ../components/xmini_rtc/include ../components/tirtc_sdk/include ../components/demo_platform/include
        "${ESP_IDF_PATH}/components/json/cJSON")
    target_compile_options(xmini_business_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_link_options(xmini_business_tests PRIVATE -Wl,--wrap=malloc -Wl,--wrap=free)
    add_test(NAME xmini_business COMMAND xmini_business_tests)
    add_executable(xmini_memtrace_tests ../components/xmini_memtrace/tests/test_memtrace.c
        ../components/xmini_memtrace/xmini_memtrace.c)
    target_include_directories(xmini_memtrace_tests PRIVATE ../components/xmini_memtrace/include
        ../components/xmini_memtrace/tests/stubs ../components/xmini_rtc/tests/stubs)
    target_compile_definitions(xmini_memtrace_tests PRIVATE XMINI_MEMTRACE_ENABLED=1)
    target_compile_options(xmini_memtrace_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME xmini_memtrace COMMAND xmini_memtrace_tests)
    find_package(Threads REQUIRED)
    add_executable(xmini_heap_failure_tests ../components/xmini_memtrace/tests/test_heap_failure.c
        ../components/xmini_memtrace/xmini_heap_failure.c)
    target_include_directories(xmini_heap_failure_tests PRIVATE ../components/xmini_memtrace/tests/heap_stubs
        ../components/xmini_memtrace/include)
    target_compile_definitions(xmini_heap_failure_tests PRIVATE _DEFAULT_SOURCE=1)
    target_compile_options(xmini_heap_failure_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    target_link_libraries(xmini_heap_failure_tests PRIVATE Threads::Threads)
    foreach(mode basic register-fail concurrent guarded null no-task boundary truncated long
            concurrent-mixed context-0-0 context-0-1 context-1-0 context-1-1 deferred deferred-safe owned-task)
        add_test(NAME xmini_heap_failure_${mode} COMMAND xmini_heap_failure_tests ${mode})
    endforeach()
    add_executable(xmini_runtime_wifi_tests test_runtime_wifi.c ../components/xmini_settings/xmini_settings_state.c)
    target_include_directories(xmini_runtime_wifi_tests PRIVATE runtime_stubs platform_stubs
        settings_stubs ../components/demo_platform/include ../components/xmini_settings/include)
    target_compile_options(xmini_runtime_wifi_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME xmini_runtime_wifi COMMAND xmini_runtime_wifi_tests)
    add_executable(xmini_product_tests test_product.c ../components/xmini_ui/xmini_guide.c
        ../components/xmini_ui/xmini_canvas.c ../components/xmini_settings/xmini_settings_state.c)
    target_include_directories(xmini_product_tests PRIVATE product_stubs platform_stubs
        ../components/xmini_audio/include ../components/xmini_ui/include
        settings_stubs ../components/xmini_settings/include
        ../components/demo_platform/include ../components/wifi_provisioning/include)
    target_compile_options(xmini_product_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME xmini_product COMMAND xmini_product_tests)
    foreach(module platform_state platform_control platform_routes wifi_credentials)
        add_executable(xmini_${module}_tests test_${module}.c)
        target_include_directories(xmini_${module}_tests PRIVATE platform_stubs
            ../components/demo_platform ../components/demo_platform/include ../components/wifi_provisioning)
        target_compile_options(xmini_${module}_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
        add_test(NAME xmini_${module} COMMAND xmini_${module}_tests)
    endforeach()
    add_executable(xmini_platform_network_tests test_platform_network.c)
    target_include_directories(xmini_platform_network_tests PRIVATE network_stubs platform_stubs
        ../components/demo_platform/include "${ESP_IDF_PATH}/components/json/cJSON")
    target_compile_options(xmini_platform_network_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME xmini_platform_network COMMAND xmini_platform_network_tests)
    add_executable(xmini_platform_task_metrics_tests test_platform_task_metrics.c
        ../components/demo_platform/platform_task_metrics.c)
    target_include_directories(xmini_platform_task_metrics_tests PRIVATE task_metrics_stubs platform_stubs
        ../components/demo_platform ../components/demo_platform/include)
    target_compile_options(xmini_platform_task_metrics_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME xmini_platform_task_metrics COMMAND xmini_platform_task_metrics_tests)
    add_executable(xmini_platform_request_tests test_platform_request.c
        "${ESP_IDF_PATH}/components/json/cJSON/cJSON.c")
    target_include_directories(xmini_platform_request_tests PRIVATE platform_stubs
        ../components/demo_platform ../components/demo_platform/include "${ESP_IDF_PATH}/components/json/cJSON")
    target_compile_options(xmini_platform_request_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME xmini_platform_request COMMAND xmini_platform_request_tests)
    set(XMINI_HTTP_METADATA_ARGS --compiler "${CMAKE_C_COMPILER}" --idf "${ESP_IDF_PATH}")
    if(XMINI_HOST_SANITIZERS)
        list(APPEND XMINI_HTTP_METADATA_ARGS --sanitize)
    endif()
    add_test(NAME xmini_platform_http_metadata COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_platform_http_metadata.py ${XMINI_HTTP_METADATA_ARGS})
    add_test(NAME xmini_heapcap_status COMMAND ${Python3_EXECUTABLE}
        ${CMAKE_CURRENT_SOURCE_DIR}/test_heapcap_status.py ${XMINI_HTTP_METADATA_ARGS})
    add_executable(xmini_settings_tests test_settings.c ../components/xmini_settings/xmini_settings_state.c)
    target_include_directories(xmini_settings_tests PRIVATE platform_stubs settings_stubs
        ../components/xmini_settings/include)
    target_compile_options(xmini_settings_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
    add_test(NAME xmini_settings COMMAND xmini_settings_tests)
    if(EXISTS "${ESP_IDF_PATH}/components/json/cJSON/cJSON.c")
        add_executable(xmini_group_tests ../components/xmini_group/tests/test_group.c
            "${ESP_IDF_PATH}/components/json/cJSON/cJSON.c")
        target_include_directories(xmini_group_tests PRIVATE ../components/xmini_group/tests/stubs audio_stubs ../components/demo_platform/include
            ../components/xmini_group/include ../components/xmini_rtc/include "${ESP_IDF_PATH}/components/json/cJSON")
        target_compile_options(xmini_group_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
        add_test(NAME xmini_group COMMAND xmini_group_tests)
    endif()
    set_tests_properties(xmini_rtc xmini_rx_repacketize xmini_business xmini_memtrace xmini_group PROPERTIES LABELS session)
else()
    message(WARNING "MSVC runs the portable UI/command/guide subset only; use WSL for the full host suite")
endif()
add_executable(xmini_ui_tests test_ui.c
    ../components/xmini_ui/xmini_key.c
    ../components/xmini_ui/xmini_ui.c
    ../components/xmini_ui/xmini_canvas.c)
target_include_directories(xmini_ui_tests PRIVATE ../components/xmini_ui/include)
if(MSVC)
    target_compile_options(xmini_ui_tests PRIVATE /W4 /UNDEBUG)
else()
    target_compile_options(xmini_ui_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
endif()
add_test(NAME xmini_ui COMMAND xmini_ui_tests)

add_executable(xmini_command_tests test_command.c ../main/xmini_command.c)
target_include_directories(xmini_command_tests PRIVATE ../main)
if(MSVC)
    target_compile_options(xmini_command_tests PRIVATE /W4 /UNDEBUG)
else()
    target_compile_options(xmini_command_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
endif()
add_test(NAME xmini_command COMMAND xmini_command_tests)

add_executable(xmini_guide_tests test_guide.c ../components/xmini_ui/xmini_guide.c ../components/xmini_ui/xmini_canvas.c)
target_include_directories(xmini_guide_tests PRIVATE ../components/xmini_ui/include)
if(MSVC)
    target_compile_options(xmini_guide_tests PRIVATE /W4 /UNDEBUG)
else()
    target_compile_options(xmini_guide_tests PRIVATE -Wall -Wextra -Werror -UNDEBUG)
endif()
add_test(NAME xmini_guide COMMAND xmini_guide_tests)

get_property(XMINI_HOST_TESTS DIRECTORY PROPERTY TESTS)
set_tests_properties(${XMINI_HOST_TESTS} PROPERTIES TIMEOUT 30)
if(XMINI_HOST_SANITIZERS)
    set_tests_properties(${XMINI_HOST_TESTS} PROPERTIES ENVIRONMENT
        "ASAN_OPTIONS=detect_leaks=1:halt_on_error=1;UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")
endif()
