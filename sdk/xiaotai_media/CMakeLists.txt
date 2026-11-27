# SPDX-License-Identifier: MIT
# XiaoTai media engine: the audio pipelines shared by every product
# (xiaotai/media.h), their extension points (xiaotai/media_capture.h,
# xiaotai/media_codec.h), the network audio types (xiaotai/media_format.h) and
# the downlink buffer (xiaotai/media_downlink.h).
#
# The portable core (src/) owns sessions, generations, pacing, mixing, sounds,
# recording, the pre-roll and every counter, and carries the implementations
# that need no library (G.711, echo gate, pass-through, high-pass, noise gate,
# resamplers, WAV parsing). Implementations that wrap a platform library (echo
# cancellers, gain control, Opus) live in esp-idf/ and register themselves
# through the same registries; host tests register fakes.
#
# Prepared objects and buffer sizes are owned by media.c; session_runtime.c
# coordinates the independent capture, uplink and playout task owners.
# Network, codecs and optional console features are selected at the edge;
# the portable admission and timing logic does not name any product.
#
# Optional part, compiled when the product selects the manifest name (build
# property XIAOTAI_SELECTED; the host build takes every one that exists):
#   src/boot_net.c    "net" (adds xiaotai_net): start asks
#                     xiaotai_net_provisioning_boot() and, in a provisioning
#                     boot, builds only the sound playout (xiaotai/media.h)
#   src/boot_none.c   without "net": never a provisioning boot
# The public headers include no optional component.
#
# Option parts (work package M11; xiaotai/media_config.h). Each holds only the
# AT handlers of its option, all named with the prefix the manifest gives as
# "symbol", so the post-link check finds none when the option is off. The
# ESP-IDF branch adds a file when its CONFIG_ symbol is set; host tests compile
# all three and check registration with the symbols defined and undefined.
#   src/trace.c       CONFIG_XIAOTAI_MEDIA_TRACE           xiaotai_media_trace_
#   src/at_test.c     CONFIG_XIAOTAI_MEDIA_TEST_COMMANDS   xiaotai_media_at_test_
#   src/at_sound.c    CONFIG_XIAOTAI_MEDIA_SOUND_COMMANDS  xiaotai_media_at_sound_
set(sources src/media.c src/session.c src/session_runtime.c src/session_events.c src/registry.c src/downlink.c
            src/g711.c src/resample.c src/sound_decode.c src/play_api.c src/clip_playout.c src/preroll.c
            src/preroll_api.c src/downlink_rx.c src/pacer.c src/playout.c src/loudness.c src/record.c
            src/play_stream.c src/play_stream_api.c src/audio_workers.c src/topology.c
            src/diag.c src/commands.c
            src/processors/echo_gate.c src/processors/passthrough.c src/processors/frontend_pairing.c
            src/effects/highpass.c src/effects/noise_gate.c)
set(private_requires xiaotai_eventlog xiaotai_at)
if(ESP_PLATFORM)
    idf_build_get_property(selected XIAOTAI_SELECTED)
elseif(XIAOTAI_HOST_PRODUCT_SELECTION)
    set(selected ${XIAOTAI_SELECTED})
else()
    set(selected net)
endif()
if("net" IN_LIST selected)
    list(APPEND private_requires xiaotai_net)
    list(APPEND sources src/boot_net.c)
else()
    list(APPEND sources src/boot_none.c)
endif()
if(ESP_PLATFORM)
    list(APPEND sources esp-idf/media_platform.c)
    if(CONFIG_XIAOTAI_MEDIA_PROCESSOR_AEC_DUAL OR CONFIG_XIAOTAI_MEDIA_PROCESSOR_AEC_SINGLE_FRONTEND)
        list(APPEND sources esp-idf/aec_frontend.c)
    endif()
    if(CONFIG_XIAOTAI_MEDIA_PROCESSOR_AEC_SINGLE)
        list(APPEND sources esp-idf/aec_single.c)
    endif()
    if(CONFIG_XIAOTAI_MEDIA_EFFECT_AGC)
        list(APPEND sources esp-idf/agc.c)
    endif()
    if(CONFIG_XIAOTAI_MEDIA_CODEC_OPUS_ENCODER OR CONFIG_XIAOTAI_MEDIA_CODEC_OPUS_DECODER)
        list(APPEND sources esp-idf/opus.c)
    endif()
    if(CONFIG_XIAOTAI_MEDIA_TRACE)
        list(APPEND sources src/trace.c)
    endif()
    if(CONFIG_XIAOTAI_MEDIA_TEST_COMMANDS)
        list(APPEND sources src/at_test.c)
    endif()
    if(CONFIG_XIAOTAI_MEDIA_SOUND_COMMANDS)
        list(APPEND sources src/at_sound.c)
    endif()
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src" "esp-idf"
                           REQUIRES xiaotai_port xiaotai_framework xiaotai_hal xiaotai_sounds
                           PRIV_REQUIRES ${private_requires})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake). The event
# log (real-time reports), the AT helpers (diagnostic commands) and the network
# engine (the provisioning boot) are used by the sources only, never by the
# public headers.
xiaotai_host_require(xiaotai_port xiaotai_framework xiaotai_hal xiaotai_sounds ${private_requires})
add_library(xiaotai_media STATIC ${sources} host/media_platform.c src/trace.c src/at_test.c src/at_sound.c)
target_include_directories(xiaotai_media PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_media PUBLIC xiaotai_port xiaotai_framework xiaotai_hal xiaotai_sounds
                      PRIVATE ${private_requires} m)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
