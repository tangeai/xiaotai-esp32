# SPDX-License-Identifier: MIT
# Host tests of the sound resources.
xiaotai_host_test(sounds_index SOURCES index_test.c LINK xiaotai_sounds)
xiaotai_host_test(sounds_inspect SOURCES inspect_test.c LINK xiaotai_sounds)
xiaotai_host_test(sounds_packs SOURCES packs_test.c LINK xiaotai_sounds_packs_fixture)
if(XIAOTAI_HOST_SANITIZE)
    add_test(NAME sounds_gen_pack COMMAND "${Python3_EXECUTABLE}" -B "${CMAKE_CURRENT_SOURCE_DIR}/gen_pack_test.py"
             "${CMAKE_CURRENT_SOURCE_DIR}/..")
    set_tests_properties(sounds_gen_pack PROPERTIES LABELS xiaotai_sounds TIMEOUT 30)
endif()

# The built-in pack table (src/packs.c) with pack options switched on, against
# fake packs under the generated symbol names. The sources are compiled into
# each test, so the options apply to them and not to the component.
set(builtin_sources builtin_test.c ../src/sounds.c ../src/packs.c)
xiaotai_host_test(sounds_builtin_all SOURCES ${builtin_sources} INCLUDE_ONLY xiaotai_sounds)
target_compile_definitions(sounds_builtin_all PRIVATE
    CONFIG_XIAOTAI_SOUNDS_PACK_TONES_PCM8K=1
    CONFIG_XIAOTAI_SOUNDS_PACK_GUIDE_ZH_ALAW8K=1)
xiaotai_host_test(sounds_builtin_one SOURCES ${builtin_sources} INCLUDE_ONLY xiaotai_sounds)
target_compile_definitions(sounds_builtin_one PRIVATE
    CONFIG_XIAOTAI_SOUNDS_PACK_TONES_PCM8K=0
    CONFIG_XIAOTAI_SOUNDS_PACK_GUIDE_ZH_ALAW8K=1)
xiaotai_host_test(sounds_builtin_refused SOURCES ${builtin_sources} INCLUDE_ONLY xiaotai_sounds)
target_compile_definitions(sounds_builtin_refused PRIVATE
    CONFIG_XIAOTAI_SOUNDS_PACK_TONES_PCM8K=1
    CONFIG_XIAOTAI_SOUNDS_PACK_GUIDE_ZH_ALAW8K=1
    SOUNDS_TEST_BAD_GUIDE=1)
