# SPDX-License-Identifier: MIT
# XiaoTai sound resources: prompts, rings and voice guidance, found by name
# (xiaotai/sounds.h). The component holds and describes sounds; the media
# engine plays them. Sounds come in packs under packs/<pack>/: the audio data,
# an index (pack.json) and the C table generated from it. The product
# configuration lists the packs a build embeds; an application may register a
# pack of its own. Encodings are the port's codec vocabulary
# (xiaotai/codec.h), so the component needs no hardware abstraction.
#
# Portable sources, the same on both platforms; there is no platform code:
#   src/sounds.c   registration, lookup, enumeration, digit names
#   src/inspect.c  container check (src/wav.h: the RIFF/WAVE parser)
#   src/packs.c    xiaotai_engine_sounds_init: registers the packs whose
#                  option (xiaotai/sounds_config.h) is on
set(sources src/sounds.c src/inspect.c src/packs.c)
set(pack_names tones_pcm8k guide_zh_alaw8k)

function(xiaotai_generate_sound_pack pack mode python out)
    set(dir "${CMAKE_CURRENT_SOURCE_DIR}/packs/${pack}")
    set(manifest "${dir}/pack.json")
    set(generator "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_pack.py")
    set(generated "${CMAKE_CURRENT_BINARY_DIR}/sound_pack_${pack}.c")
    # The generator checks that this exact set is declared and referenced in
    # the manifest. The pack prefix also makes IDF's basename symbols unique.
    file(GLOB assets LIST_DIRECTORIES false CONFIGURE_DEPENDS "${dir}/xt_sound_${pack}_*")
    add_custom_command(OUTPUT "${generated}"
        COMMAND "${python}" -B "${generator}" --mode "${mode}" "${manifest}" "${generated}"
        DEPENDS "${generator}" "${manifest}" ${assets}
        COMMENT "Checking XiaoTai sound pack ${pack}"
        VERBATIM)
    if(mode STREQUAL "idf")
        foreach(asset IN LISTS assets)
            target_add_binary_data(${COMPONENT_LIB} "${asset}" BINARY DEPENDS "${generated}")
        endforeach()
    endif()
    set(${out} "${generated}" PARENT_SCOPE)
endfunction()

if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
                           REQUIRES xiaotai_port)
    idf_build_get_property(python PYTHON)
    foreach(pack IN LISTS pack_names)
        string(TOUPPER "${pack}" option_suffix)
        if(CONFIG_XIAOTAI_SOUNDS_PACK_${option_suffix})
            xiaotai_generate_sound_pack("${pack}" idf "${python}" generated)
            target_sources(${COMPONENT_LIB} PRIVATE "${generated}")
        endif()
    endforeach()
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port)
add_library(xiaotai_sounds STATIC ${sources})
target_include_directories(xiaotai_sounds PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_sounds PUBLIC xiaotai_port)
foreach(pack IN LISTS pack_names)
    string(TOUPPER "${pack}" option_suffix)
    set(option_name "CONFIG_XIAOTAI_SOUNDS_PACK_${option_suffix}")
    option(${option_name} "Embed sound pack ${pack} in the host component" OFF)
    if(${option_name} OR XIAOTAI_HOST_TESTS)
        if(NOT Python3_Interpreter_FOUND)
            find_package(Python3 REQUIRED COMPONENTS Interpreter)
        endif()
        xiaotai_generate_sound_pack("${pack}" host "${Python3_EXECUTABLE}" generated)
        add_library(xiaotai_sound_data_${pack} STATIC "${generated}")
        target_include_directories(xiaotai_sound_data_${pack} PRIVATE include)
        target_link_libraries(xiaotai_sound_data_${pack} PRIVATE xiaotai_port)
        list(APPEND host_fixture_packs xiaotai_sound_data_${pack})
        list(APPEND host_fixture_options "${option_name}=1")
        if(${option_name})
            target_link_libraries(xiaotai_sounds PRIVATE xiaotai_sound_data_${pack})
            target_compile_definitions(xiaotai_sounds PUBLIC "${option_name}=1")
        endif()
    endif()
endforeach()
if(XIAOTAI_HOST_TESTS)
    # Exercise real packs even when no product opts into them. Keep them out
    # of the default component and run the same registry and selection code.
    add_library(xiaotai_sounds_packs_fixture STATIC ${sources})
    target_include_directories(xiaotai_sounds_packs_fixture PUBLIC include PRIVATE src)
    target_link_libraries(xiaotai_sounds_packs_fixture PUBLIC xiaotai_port PRIVATE ${host_fixture_packs})
    target_compile_definitions(xiaotai_sounds_packs_fixture PRIVATE ${host_fixture_options})
    add_subdirectory(test)
endif()
