# SPDX-License-Identifier: MIT
# Opt in to the Opus ring probe on the real tests/sdk/product graph using CMake 3.19+
# -DCMAKE_PROJECT_xiaotai_product_host_INCLUDE=<absolute path to this file>.
set(_opus_ring_probe_dir "${CMAKE_CURRENT_LIST_DIR}")

function(attach_opus_product_ring_probe)
    if(NOT XIAOTAI_HOST_PRODUCT MATCHES "^xiaotai-(waveshare-p4-rev3|wt99p461-s1-1v1)$" OR
       NOT XIAOTAI_HOST_PRODUCT_CASE STREQUAL "default" OR
       NOT CONFIG_XIAOTAI_MEDIA_CODEC_OPUS_DECODER)
        message(FATAL_ERROR "The Opus ring probe requires a P4 default graph with Opus enabled")
    endif()
    set(headers "${XIAOTAI_HOST_OPUS_ADAPTER_HEADERS}")
    set(lib "${XIAOTAI_HOST_OPUS_ADAPTER_LIBRARY}")
    if(NOT EXISTS "${headers}/decoder/impl/esp_opus_dec.h" OR NOT EXISTS "${lib}")
        message(FATAL_ERROR "Pass the same pinned headers and libopus as media_opus_adapter")
    endif()
    set(bench "${_opus_ring_probe_dir}/opus_adapter_test.c")
    set_source_files_properties("${bench}" PROPERTIES COMPILE_DEFINITIONS "main=media_opus_adapter_bench_main")
    target_sources(product_host PRIVATE "${bench}" "${_opus_ring_probe_dir}/../esp-idf/opus.c")
    target_include_directories(product_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${headers}"
                               "${headers}/decoder" "${_opus_ring_probe_dir}/../src"
                               "${_opus_ring_probe_dir}/../../../tests/regression")
    target_link_libraries(product_host PRIVATE "${lib}")
    target_link_options(product_host PRIVATE "LINKER:--wrap=xiaotai_engine_media_init")
    if(XIAOTAI_HOST_OPUS_SCENES AND (XIAOTAI_HOST_PCMA_ACTIVE_SESSIONS OR XIAOTAI_HOST_PCMA_SESSIONS))
        target_sources(product_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/product_opus_scene.c")
        target_compile_definitions(product_host PRIVATE XIAOTAI_TEST_OPUS_SCENES=1)
    endif()
    if(XIAOTAI_HOST_PCMA_ACTIVE_SESSIONS)
        target_sources(product_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/product_pcma_active_sessions.c")
        target_link_options(product_host PRIVATE
            "LINKER:--wrap=xiaotai_start,--wrap=xiaotai_rtc_send,--wrap=xiaotai_rtc_subscribe")
        foreach(scene h5 device wechat)
            product_test("pcma_${scene}_legacy" start)
            set_property(TEST "product_${product_id}_pcma_${scene}_legacy"
                         APPEND PROPERTY ENVIRONMENT "XIAOTAI_HOST_PCMA_ACTIVE_SCENE=${scene}")
            if(XIAOTAI_HOST_OPUS_SCENES)
                product_test("opus_${scene}_legacy" start)
                set_property(TEST "product_${product_id}_opus_${scene}_legacy" APPEND PROPERTY ENVIRONMENT
                             "XIAOTAI_HOST_PCMA_ACTIVE_SCENE=${scene}" "XIAOTAI_HOST_OPUS_SCENE=1")
            endif()
        endforeach()
    elseif(XIAOTAI_HOST_PCMA_SESSIONS)
        target_sources(product_host PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/product_pcma_sessions.c")
        target_include_directories(product_host PRIVATE "${_opus_ring_probe_dir}")
        target_link_options(product_host PRIVATE "LINKER:--wrap=xiaotai_start,--wrap=xiaotai_rtc_send")
        product_test(pcma_legacy_sessions start)
        if(XIAOTAI_HOST_OPUS_SCENES)
            foreach(scene ai room)
                product_test("opus_${scene}_legacy" start)
                set_property(TEST "product_${product_id}_opus_${scene}_legacy"
                             APPEND PROPERTY ENVIRONMENT "XIAOTAI_HOST_OPUS_SCENE=${scene}")
            endforeach()
        endif()
    else()
        target_sources(product_host PRIVATE "${_opus_ring_probe_dir}/opus_product_ring_test.c")
        target_link_options(product_host PRIVATE "LINKER:--wrap=xiaotai_net_host_wait_calls,--wrap=xiaotai_time_us")
        product_test(opus_ring_legacy start)
    endif()
endfunction()

cmake_language(DEFER CALL attach_opus_product_ring_probe)
