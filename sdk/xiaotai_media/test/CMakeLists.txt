# SPDX-License-Identifier: MIT
# Host tests of the media engine. Tests of internal building blocks include
# the component's src/ directory.
set(media_src "${CMAKE_CURRENT_SOURCE_DIR}/../src")
set(XIAOTAI_HOST_OPUS_ADAPTER_HEADERS "" CACHE PATH "Pinned esp_audio_codec include directory, for the adapter test only")
set(XIAOTAI_HOST_OPUS_ADAPTER_LIBRARY "" CACHE FILEPATH "Pinned host libopus, for the adapter test only")
if(XIAOTAI_HOST_OPUS_ADAPTER_HEADERS OR XIAOTAI_HOST_OPUS_ADAPTER_LIBRARY)
    if(NOT EXISTS "${XIAOTAI_HOST_OPUS_ADAPTER_HEADERS}/decoder/impl/esp_opus_dec.h" OR
       NOT EXISTS "${XIAOTAI_HOST_OPUS_ADAPTER_LIBRARY}")
        message(FATAL_ERROR "The Opus adapter host test requires both the pinned headers and host library")
    endif()
    xiaotai_host_test(media_opus_adapter SOURCES opus_adapter_test.c ../esp-idf/opus.c
        LINK xiaotai_media INCLUDE ${media_src} "${XIAOTAI_HOST_OPUS_ADAPTER_HEADERS}"
        "${XIAOTAI_HOST_OPUS_ADAPTER_HEADERS}/decoder" ../../../tests/regression)
    target_compile_definitions(media_opus_adapter PRIVATE CONFIG_XIAOTAI_MEDIA_CODEC_OPUS_DECODER=1)
    target_link_libraries(media_opus_adapter PRIVATE "${XIAOTAI_HOST_OPUS_ADAPTER_LIBRARY}")
    if(TEST media_opus_adapter)
        add_test(NAME media_opus_adapter_metadata COMMAND media_opus_adapter metadata)
        set_tests_properties(media_opus_adapter_metadata PROPERTIES LABELS xiaotai_media TIMEOUT 20)
    endif()
endif()
xiaotai_host_test(media_downlink SOURCES downlink_test.c fixed_speaker.c LINK xiaotai_media INCLUDE ${media_src})
xiaotai_host_test(media_playout_storage SOURCES playout_storage_test.c fixed_speaker.c
                  LINK xiaotai_media INCLUDE ${media_src})
target_link_options(media_playout_storage PRIVATE "LINKER:--wrap=xiaotai_mem_alloc"
                    "LINKER:--wrap=xiaotai_mem_calloc" "LINKER:--wrap=xiaotai_mem_has_external"
                    "LINKER:--wrap=xiaotai_time_us" "LINKER:--wrap=xiaotai_media_param"
                    "LINKER:--wrap=media_rx_format_at")
xiaotai_host_test(media_playout_legacy_vectors SOURCES playout_migration_test.c fixed_speaker.c
                  LINK xiaotai_media INCLUDE ${media_src})
target_link_options(media_playout_legacy_vectors PRIVATE "LINKER:--wrap=xiaotai_time_us"
                    "LINKER:--wrap=xiaotai_media_param" "LINKER:--wrap=media_rx_format_at"
                    "LINKER:--wrap=media_pacer_timed_delta")
xiaotai_host_test(media_pacer SOURCES pacer_test.c LINK xiaotai_media INCLUDE ${media_src})
xiaotai_host_test(media_codec SOURCES codec_test.c fixed_speaker.c LINK xiaotai_media)
xiaotai_host_test(media_registry SOURCES registry_test.c LINK xiaotai_media INCLUDE ${media_src})
xiaotai_host_test(media_require SOURCES require_test.c LINK xiaotai_media INCLUDE ${media_src})
target_link_options(media_require PRIVATE "LINKER:--wrap=xiaotai_mem_has_external")
xiaotai_host_test(media_boot_sizing SOURCES boot_sizing_test.c fixed_speaker.c LINK xiaotai_media INCLUDE ${media_src})
target_link_options(media_boot_sizing PRIVATE "LINKER:--wrap=xiaotai_net_provisioning_boot")
xiaotai_host_test(media_boot_without_net SOURCES boot_without_net_test.c fixed_speaker.c
                  ../src/boot_none.c LINK xiaotai_media INCLUDE ${media_src})
if(TARGET xiaotai_sounds_packs_fixture)
    xiaotai_host_test(media_sound_decode SOURCES sound_decode_test.c fixed_speaker.c
                      LINK xiaotai_media xiaotai_sounds_packs_fixture INCLUDE ${media_src})
    xiaotai_host_test(media_provisioning_playback SOURCES provisioning_playback_test.c
                      LINK xiaotai_media xiaotai_sounds_packs_fixture)
    target_link_options(media_provisioning_playback PRIVATE "LINKER:--wrap=xiaotai_net_provisioning_boot"
                         "LINKER:--wrap=xiaotai_task_create" "LINKER:--wrap=xiaotai_mem_alloc")
    find_library(XIAOTAI_TEST_OPUS_LIBRARY NAMES opus libopus.so.0)
    if(XIAOTAI_TEST_OPUS_LIBRARY)
        target_link_libraries(media_provisioning_playback PRIVATE ${XIAOTAI_TEST_OPUS_LIBRARY})
        target_compile_definitions(media_provisioning_playback PRIVATE XIAOTAI_TEST_WITH_LIBOPUS=1)
    endif()
endif()
xiaotai_host_test(media_session_control SOURCES session_control_test.c fixed_speaker.c LINK xiaotai_media
                  INCLUDE ${media_src})
xiaotai_host_test(media_session_events SOURCES session_events_test.c LINK xiaotai_media INCLUDE ${media_src})
xiaotai_host_test(media_startup SOURCES startup_test.c LINK xiaotai_media)
target_link_options(media_startup PRIVATE "LINKER:--wrap=xiaotai_event_emit" "LINKER:--wrap=xiaotai_task_create")
xiaotai_host_test(media_clip_playout SOURCES clip_playout_test.c fixed_speaker.c
                  LINK xiaotai_media INCLUDE ${media_src})
xiaotai_host_test(media_play_stream SOURCES play_stream_test.c LINK xiaotai_media xiaotai_eventlog)
xiaotai_host_test(media_play_mix SOURCES play_mix_test.c LINK xiaotai_media xiaotai_eventlog)
xiaotai_host_test(media_preroll_core SOURCES preroll_test.c LINK xiaotai_media INCLUDE ${media_src})
target_link_options(media_preroll_core PRIVATE "LINKER:--wrap=xiaotai_mutex_lock")
foreach(test media_record media_record_progress media_preroll media_record_legacy_window)
    xiaotai_host_test(${test} SOURCES record_test.c LINK xiaotai_media INCLUDE ${media_src})
    target_link_options(${test} PRIVATE "LINKER:--wrap=xiaotai_net_provisioning_boot"
                        "LINKER:--wrap=xiaotai_task_create"
                        "LINKER:--wrap=xiaotai_mem_has_external")
endforeach()
target_compile_definitions(media_record_progress PRIVATE XIAOTAI_TEST_RECORD_PROGRESS=1)
target_compile_definitions(media_record_legacy_window PRIVATE XIAOTAI_TEST_RECORD_LEGACY=1)
add_test(NAME media_record_legacy_agc_window COMMAND media_record_legacy_window agc)
set_tests_properties(media_record_legacy_agc_window PROPERTIES LABELS xiaotai_media TIMEOUT 20)
target_compile_definitions(media_preroll PRIVATE XIAOTAI_TEST_PREROLL=1)
xiaotai_host_test(media_session_runtime SOURCES session_runtime_test.c LINK xiaotai_media)
target_link_options(media_session_runtime PRIVATE "LINKER:--wrap=xiaotai_net_provisioning_boot"
                    "LINKER:--wrap=media_downlink_pop_tagged" "LINKER:--wrap=xiaotai_time_us" "LINKER:--wrap=memcpy"
                    "LINKER:--wrap=media_rx_writers")
add_test(NAME media_file_output_error COMMAND media_session_runtime file-output-error)
set_tests_properties(media_file_output_error PROPERTIES LABELS xiaotai_media TIMEOUT 20)
add_test(NAME media_deferred_codec_mute COMMAND media_session_runtime deferred-codec-mute)
add_test(NAME media_immediate_codec_mute COMMAND media_session_runtime immediate-codec-mute)
set_tests_properties(media_deferred_codec_mute media_immediate_codec_mute PROPERTIES LABELS xiaotai_media TIMEOUT 20)
xiaotai_host_test(media_folded SOURCES folded_audio_test.c LINK xiaotai_media xiaotai_eventlog
                  INCLUDE ${media_src} TIMEOUT 45)
target_link_options(media_folded PRIVATE "LINKER:--wrap=xiaotai_net_provisioning_boot" "LINKER:--wrap=xiaotai_mem_has_external"
                     "LINKER:--wrap=xiaotai_task_create" "LINKER:--wrap=xiaotai_queue_init"
                     "LINKER:--wrap=xiaotai_queue_receive" "LINKER:--wrap=xiaotai_time_us")
add_test(NAME media_folded_delegated COMMAND media_folded delegated)
set_tests_properties(media_folded_delegated PROPERTIES LABELS xiaotai_media TIMEOUT 20)
add_test(NAME media_folded_blocked_capture COMMAND media_folded blocked_capture)
set_tests_properties(media_folded_blocked_capture PROPERTIES LABELS xiaotai_media TIMEOUT 20)
foreach(case compact compact_decoder compact_override)
    add_test(NAME media_folded_${case} COMMAND media_folded ${case})
    set_tests_properties(media_folded_${case} PROPERTIES LABELS xiaotai_media TIMEOUT 45)
endforeach()
foreach(case tx_age tx_age_off)
    add_test(NAME media_folded_${case} COMMAND media_folded ${case})
    set_tests_properties(media_folded_${case} PROPERTIES LABELS xiaotai_media TIMEOUT 20)
endforeach()
# These two drive the simulated codec clock for ten seconds of real time and
# count every missed hardware deadline: run them alone, so the rest of the
# suite does not take their CPU.
if(TEST media_folded)
    set_tests_properties(media_folded media_folded_compact PROPERTIES RUN_SERIAL TRUE)
endif()
if(XIAOTAI_HOST_SANITIZE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME media_memory_tool COMMAND "${Python3_EXECUTABLE}" -B
             "${CMAKE_CURRENT_SOURCE_DIR}/../../../tools/xt/test_media_memory.py")
    set_tests_properties(media_memory_tool PROPERTIES LABELS xiaotai_media)
endif()
xiaotai_host_test(media_audio_workers SOURCES audio_workers_test.c LINK xiaotai_media xiaotai_at xiaotai_eventlog INCLUDE ${media_src})
target_link_options(media_audio_workers PRIVATE "LINKER:--wrap=xiaotai_task_create")
xiaotai_host_test(media_audio_workers_async SOURCES audio_workers_test.c LINK xiaotai_media xiaotai_at xiaotai_eventlog INCLUDE ${media_src})
target_compile_definitions(media_audio_workers_async PRIVATE AUDIO_WORKERS_ASYNC=1)
target_link_options(media_audio_workers_async PRIVATE "LINKER:--wrap=xiaotai_task_create")
add_test(NAME media_capture_window COMMAND media_audio_workers window)
add_test(NAME media_capture_window_off COMMAND media_audio_workers window_off)
add_test(NAME media_capture_window_async COMMAND media_audio_workers_async window)
set_tests_properties(media_capture_window media_capture_window_off media_capture_window_async PROPERTIES LABELS xiaotai_media)
xiaotai_host_test(media_audio_workers_p4_pcma SOURCES audio_workers_test.c LINK xiaotai_media INCLUDE ${media_src})
target_compile_definitions(media_audio_workers_p4_pcma PRIVATE AUDIO_WORKERS_PCMA_LEGACY=1)
target_link_options(media_audio_workers_p4_pcma PRIVATE "LINKER:--wrap=xiaotai_task_create,--wrap=xiaotai_queue_receive")
foreach(transition start generation mute)
    add_test(NAME media_audio_workers_pcma_${transition}_wait COMMAND media_audio_workers_p4_pcma ${transition}-wait)
    set_tests_properties(media_audio_workers_pcma_${transition}_wait PROPERTIES LABELS xiaotai_media TIMEOUT 20)
endforeach()
xiaotai_host_test(media_g711 SOURCES g711_test.c fixed_speaker.c LINK xiaotai_media INCLUDE ${media_src})
xiaotai_host_test(media_resample SOURCES resample_test.c LINK xiaotai_media m INCLUDE ${media_src})
xiaotai_host_test(media_level SOURCES level_test.c LINK xiaotai_media m INCLUDE ${media_src})
xiaotai_host_test(media_frame_stream SOURCES frame_stream_test.c LINK xiaotai_media INCLUDE ${media_src})
xiaotai_host_test(media_agc_stack SOURCES agc_stack_test.c ../esp-idf/agc.c
                  LINK xiaotai_media m INCLUDE ${media_src} agc_stubs)
target_link_options(media_agc_stack PRIVATE "LINKER:--wrap=xiaotai_media_param" "LINKER:--wrap=xiaotai_mem_calloc")
xiaotai_host_test(media_agc_gap SOURCES agc_gap_probe.c ../esp-idf/agc.c
                  LINK xiaotai_media m INCLUDE ${media_src} agc_stubs)
add_test(NAME media_agc_gap_short COMMAND media_agc_gap 256)
set_tests_properties(media_agc_gap_short PROPERTIES LABELS xiaotai_media TIMEOUT 20)
xiaotai_host_test(media_echo_gate SOURCES echo_gate_test.c fixed_speaker.c LINK xiaotai_media m INCLUDE ${media_src})
xiaotai_host_test(media_highpass SOURCES highpass_test.c fixed_speaker.c LINK xiaotai_media INCLUDE ${media_src})
xiaotai_host_test(media_noise_gate SOURCES noise_gate_test.c fixed_speaker.c
                  LINK xiaotai_media xiaotai_eventlog m INCLUDE ${media_src})
xiaotai_host_test(media_passthrough SOURCES passthrough_test.c LINK xiaotai_media INCLUDE ${media_src})
target_link_options(media_passthrough PRIVATE "LINKER:--wrap=xiaotai_net_provisioning_boot")
target_link_options(media_passthrough PRIVATE "LINKER:--wrap=xiaotai_task_create")
xiaotai_host_test(media_frontend_pairing SOURCES frontend_pairing_test.c LINK xiaotai_media INCLUDE ${media_src})
target_link_options(media_frontend_pairing PRIVATE "LINKER:--wrap=xiaotai_time_us")
xiaotai_host_test(media_aec_frontend_defaults SOURCES aec_frontend_defaults_test.c fixed_speaker.c
                  ../esp-idf/aec_frontend.c LINK xiaotai_media INCLUDE ${media_src} afe_stubs)
target_compile_definitions(media_aec_frontend_defaults PRIVATE CONFIG_XIAOTAI_MEDIA_PROCESSOR_AEC_SINGLE_FRONTEND=1
                           CONFIG_XIAOTAI_MEDIA_AFE_MEMORY_EXTERNAL=1)
target_link_options(media_aec_frontend_defaults PRIVATE "LINKER:--wrap=media_frontend_pairing_open")
xiaotai_host_test(media_topology SOURCES topology_test.c LINK xiaotai_media INCLUDE ${media_src})
xiaotai_host_test(media_pipeline SOURCES pipeline_test.c LINK xiaotai_media xiaotai_eventlog
                  INCLUDE ${media_src} TIMEOUT 30)
target_link_options(media_pipeline PRIVATE "LINKER:--wrap=xiaotai_net_provisioning_boot" "LINKER:--wrap=xiaotai_task_create")
include("${CMAKE_CURRENT_LIST_DIR}/m11.cmake")
