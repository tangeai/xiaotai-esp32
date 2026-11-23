# SPDX-License-Identifier: MIT
# XiaoTai port layer: interfaces (include/xiaotai/port, include/xiaotai/config.h,
# the port's own options include/xiaotai/port_config.h with Kconfig, and the
# vocabularies shared by components that must not depend on each other:
# hardware needs include/xiaotai/needs.h and codec identity
# include/xiaotai/codec.h), code shared by every port (common/), and one
# implementation per platform: esp-idf/ (every ESP chip) or host/.
#
# Implemented: os, mem (allocation, statistics, watermarks; on the host also
# the failure record and the test controls of xiaotai_mem_host.h), sys
# (firmware facts, random, restart, task list, port report and probes),
# kv (blobs and strings up to 2048 bytes, integers of every
# NVS type, erase of a key or a namespace, batches with one commit,
# asynchronous writes of each type, commit_async and read_async with their
# completions, flush), log, err, config.
# Contract only until the work packages of the component's specification land;
# their sources join the two lists below as they are written:
#   allocation observer startup: opt in only when BI-4 removes the legacy hooks
#   MQTT device backend: selected by the new product graph below
#   USB monitor replacement: opt in only once the legacy adapter is removed
# There is no network port: the network is a part (xiaotai/hal.h) driven by
# the network engine (sdk/xiaotai_net).
if(ESP_PLATFORM)
    # Requirements are also expanded by IDF in a separate CMake process.
    # Only the implementation and its options are opted in by the V3 entry.
    set(native_http_requires esp_http_client esp-tls tcp_transport http_parser)
    if(XIAOTAI_PORT_V3_PRODUCT)
        set(native_http "esp-idf/http.c" "esp-idf/http_dns.c" "common/http_rules.c")
    endif()
    set(native_mqtt)
    if(NOT CMAKE_BUILD_EARLY_EXPANSION AND XIAOTAI_PORT_V3_PRODUCT)
        set(native_mqtt "esp-idf/mqtt.c" "common/mqtt_config.c")
    endif()
    idf_component_register(
        SRCS "esp-idf/os.c" "esp-idf/mem.c" "esp-idf/sys.c" "esp-idf/diag.c" "esp-idf/kv_nvs.c" "esp-idf/digest.c"
             "esp-idf/clock.c" "esp-idf/mem_diag.c" "esp-idf/console.c" "esp-idf/log.c"
             "common/clock_gate.c" "common/clock_ntp.c" "common/console_text.c" "common/kv_worker.c"
             "common/port_report.c"
             ${native_http}
             ${native_mqtt}
        INCLUDE_DIRS "include" "esp-idf/include"
        PRIV_INCLUDE_DIRS "common"
        REQUIRES freertos heap log
        PRIV_REQUIRES esp_timer esp_hw_support esp_system esp_app_format nvs_flash mbedtls lwip spi_flash
                      esp_driver_uart esp_driver_usb_serial_jtag ${native_http_requires}
    )
    # Only the new product entry selects this, after legacy hooks and adapters are removed.
    if(XIAOTAI_PORT_V3_PRODUCT)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE XIAOTAI_PORT_NATIVE_STARTUP=1)
        if(NOT CONFIG_XIAOTAI_PORT_HTTP_DEADLINE_TRANSPORT OR
           NOT CONFIG_ESP_HTTP_CLIENT_ENABLE_CUSTOM_TRANSPORT)
            message(FATAL_ERROR "V3 HTTP requires CONFIG_XIAOTAI_PORT_HTTP_DEADLINE_TRANSPORT=y")
        endif()
        if(NOT DEFINED CONFIG_ESP_HTTP_CLIENT_EVENT_POST_TIMEOUT OR
           NOT CONFIG_ESP_HTTP_CLIENT_EVENT_POST_TIMEOUT EQUAL 0)
            message(FATAL_ERROR "V3 HTTP requires CONFIG_ESP_HTTP_CLIENT_EVENT_POST_TIMEOUT=0")
        endif()
        include("${CMAKE_CURRENT_LIST_DIR}/esp-idf/http_idf.cmake")
        xiaotai_port_configure_http()
        # The early init descriptor must be linked even in a product without AT.
        target_link_options(${COMPONENT_LIB} INTERFACE "LINKER:-u,xiaotai_mem_failure")
        include("${CMAKE_CURRENT_LIST_DIR}/esp-idf/usb_monitor/usb_monitor.cmake")
        xiaotai_port_configure_usb_monitor()
        if(NOT CMAKE_BUILD_EARLY_EXPANSION)
            # Products supply the fixed dependency; this port neither fetches
            # another MQTT version nor changes legacy requirements.
            idf_build_get_property(build_components BUILD_COMPONENTS)
            if(NOT "espressif__mqtt" IN_LIST build_components)
                message(FATAL_ERROR "V3 MQTT requires the product's locked espressif/mqtt dependency")
            endif()
            idf_component_optional_requires(PRIVATE espressif__mqtt)
            if(CONFIG_XIAOTAI_PORT_MQTT_NATIVE_OVERLAY)
                include("${CMAKE_CURRENT_LIST_DIR}/esp-idf/mqtt_idf.cmake")
                cmake_language(DEFER DIRECTORY "${PROJECT_SOURCE_DIR}" CALL xiaotai_port_configure_mqtt)
            endif()
        endif()
    endif()
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
add_library(xiaotai_port STATIC host/os.c host/mem.c host/sys.c host/diag.c host/kv_memory.c host/digest.c
            host/clock.c host/console.c host/http.c host/mqtt.c common/clock_gate.c common/clock_ntp.c
            common/console_text.c common/http_rules.c common/kv_worker.c common/mqtt_config.c)
target_include_directories(xiaotai_port PUBLIC include host/include PRIVATE common)
target_compile_definitions(xiaotai_port PRIVATE XIAOTAI_CLOCK_TEST_CONTROLS=1)
find_package(Threads REQUIRED)
target_link_libraries(xiaotai_port PUBLIC Threads::Threads)
target_compile_features(xiaotai_port PUBLIC c_std_11)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
