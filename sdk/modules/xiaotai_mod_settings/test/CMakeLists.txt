# SPDX-License-Identifier: MIT
# Host tests of the settings module: the real framework and event log on the
# host port; fakes of the board's parts, the media engine and the preferences
# service (test/fake_hal.c, test/fake_media.c). The module's sources are
# compiled into each test, so a build option can be set per test.
set(core ../src/settings.c ../src/settings_diag.c)
set(harness settings_harness.c fake_hal.c)
set(with_media ${core} ../src/with_media.c fake_media.c ${harness})

function(settings_test name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "" "" "SOURCES;DEFINES")
    xiaotai_host_test(${name} SOURCES ${arg_SOURCES}
                      INCLUDE "${CMAKE_CURRENT_SOURCE_DIR}/../src"
                      INCLUDE_ONLY xiaotai_mod_settings
                      LINK xiaotai_framework xiaotai_eventlog)
    if(arg_DEFINES)
        target_compile_definitions(${name} PRIVATE ${arg_DEFINES})
    endif()
endfunction()

settings_test(mod_settings_register SOURCES settings_register_test.c ${with_media})
# Late refusals: kept (default), taken back (option on).
settings_test(mod_settings_apply SOURCES settings_apply_test.c ${with_media})
settings_test(mod_settings_apply_take_back SOURCES settings_apply_test.c ${with_media}
              DEFINES CONFIG_XIAOTAI_SETTINGS_TAKE_BACK_LATE_REFUSAL=1)
add_test(NAME mod_settings_saved_mute COMMAND mod_settings_apply saved_mute)
add_test(NAME mod_settings_saved_mute_take_back COMMAND mod_settings_apply_take_back saved_mute)
set_tests_properties(mod_settings_saved_mute mod_settings_saved_mute_take_back
                     PROPERTIES LABELS xiaotai_mod_settings TIMEOUT 120)
# A step up: the speaker mute stays (default), is cleared (option on).
settings_test(mod_settings_volume SOURCES settings_volume_test.c ${with_media})
settings_test(mod_settings_volume_unmute SOURCES settings_volume_test.c ${with_media}
              DEFINES CONFIG_XIAOTAI_SETTINGS_VOLUME_UP_UNMUTES=1)
# A volume stored for another ceiling is converted once.
settings_test(mod_settings_volume_ceiling SOURCES settings_ceiling_test.c ${with_media})
settings_test(mod_settings_no_media SOURCES settings_no_media_test.c ${core} ../src/audio_none.c ${harness}
              DEFINES SETTINGS_TEST_NO_MEDIA)

# Explicit Wave 3 gate: enable only to exercise the old bytes through actual
# I-3 preferences. The syntax target succeeds independently; the named CTest
# must not be skipped or linked with fixture preferences when I-3 is absent.
option(XIAOTAI_SETTINGS_LEGACY_GATE "Build MS-2 with the real I-3 preference backend" OFF)
if(XIAOTAI_SETTINGS_LEGACY_GATE)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/../../../.." ABSOLUTE)
    add_library(settings_legacy_fixture OBJECT EXCLUDE_FROM_ALL settings_legacy_test.c fake_hal.c fake_media.c)
    target_compile_definitions(settings_legacy_fixture PRIVATE SETTINGS_TEST_REAL_PREFERENCES=1)
    target_link_libraries(settings_legacy_fixture PRIVATE xiaotai_mod_settings)
    add_custom_target(mod_settings_legacy_syntax DEPENDS settings_legacy_fixture)

    function(settings_legacy_test name product mode)
        set(product_file "${root}/products/${product}/product.json")
        set(glue "${CMAKE_CURRENT_BINARY_DIR}/${product}-settings-legacy.c")
        add_custom_command(OUTPUT "${glue}"
            COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_LIST_DIR}/settings_legacy_glue.py"
                    "${product}" "${mode}" "${glue}"
            DEPENDS "${product_file}" "${CMAKE_CURRENT_LIST_DIR}/settings_legacy_glue.py"
                    "${root}/scripts/sdk_selection.py" "${root}/sdk/xiaotai_identity/engine.json"
                    "${root}/sdk/modules/xiaotai_mod_settings/module.json"
            VERBATIM)
        add_library(${name}_glue OBJECT EXCLUDE_FROM_ALL "${glue}")
        target_link_libraries(${name}_glue PRIVATE xiaotai_mod_settings)
        add_dependencies(mod_settings_legacy_syntax ${name}_glue)
        xiaotai_host_test(${name} SOURCES $<TARGET_OBJECTS:settings_legacy_fixture> $<TARGET_OBJECTS:${name}_glue>
                          LINK xiaotai_mod_settings TIMEOUT 30)
        if(ARGN)
            add_test(NAME ${name}_speaker COMMAND ${name} ${ARGN})
            set_tests_properties(${name}_speaker PROPERTIES LABELS xiaotai_mod_settings TIMEOUT 30)
        endif()
    endfunction()

    settings_legacy_test(mod_settings_legacy xiaotai-xmini-c3-v3 xmini)
    settings_legacy_test(mod_settings_legacy_szpi xiaotai-szpi-s3 panel speaker_strict)
    settings_legacy_test(mod_settings_legacy_core xiaotai-cores3-lite panel speaker_strict)
    settings_legacy_test(mod_settings_legacy_waveshare xiaotai-waveshare-p4-rev3 panel speaker_snapshot_default)
    settings_legacy_test(mod_settings_legacy_wt99 xiaotai-wt99p461-s1-1v1 door_panel speaker_snapshot_default)
    add_test(NAME mod_settings_legacy_szpi_volume COMMAND mod_settings_legacy_szpi volume_strict)
    add_test(NAME mod_settings_legacy_core_volume COMMAND mod_settings_legacy_core volume_strict)
    add_test(NAME mod_settings_legacy_waveshare_volume COMMAND mod_settings_legacy_waveshare volume_default)
    set_tests_properties(mod_settings_legacy_szpi_volume mod_settings_legacy_core_volume
                         mod_settings_legacy_waveshare_volume PROPERTIES LABELS xiaotai_mod_settings TIMEOUT 30)
endif()
