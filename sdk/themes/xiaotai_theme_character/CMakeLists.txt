# SPDX-License-Identifier: MIT
# XiaoTai on-screen character (xiaotai/theme_character.h): an actor of the
# colour theme kit for large displays. The character itself (src/chr_mind.c,
# src/chr_draw.c, src/chr_raster.c) is portable C without a drawing library;
# src/chr_actor.c puts it on an LVGL page, src/chr_director.c carries the
# device's events to it. The sprites are assets/character.pack, baked by
# tools/character/bake.py; the firmware carries it behind its image.
#
# Optional part, compiled when the product selects the manifest name (build
# property XIAOTAI_SELECTED; the host build takes it when it exists):
#   src/with_media.c   "media": the mouth and ears follow the loudness the
#                      media engine publishes
#   src/media_none.c   without it they follow the view's phase and level
set(core src/chr_raster.c src/chr_mind.c src/chr_draw.c)
set(sources src/chr_unit.c src/chr_director.c)
set(requires xiaotai_port xiaotai_framework xiaotai_hal xiaotai_ui xiaotai_at xiaotai_theme_lcd_kit)
if(ESP_PLATFORM)
    idf_build_get_property(selected XIAOTAI_SELECTED)
elseif(XIAOTAI_HOST_PRODUCT_SELECTION)
    set(selected ${XIAOTAI_SELECTED})
else()
    set(selected media)
endif()
if("media" IN_LIST selected)
    list(APPEND requires xiaotai_media)
    list(APPEND sources src/with_media.c)
else()
    list(APPEND sources src/media_none.c)
endif()
if(ESP_PLATFORM)
    idf_component_register(SRCS ${core} ${sources} src/chr_actor.c src/chr_shelf.c esp-idf/chr_pack_idf.c
                           INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
                           REQUIRES ${requires} PRIV_REQUIRES lvgl esp_partition app_update)
    # The pack is not linked in: it follows the application image in its file (the product project
    # attaches it, scripts/image_trailer.py) and is read from flash picture by picture.
    idf_build_set_property(XIAOTAI_IMAGE_TRAILER "${CMAKE_CURRENT_LIST_DIR}/assets/character.pack")
    # Drawing code: built for speed on every target, as the kit's is.
    target_compile_options(${COMPONENT_LIB} PRIVATE -O2)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_theme_character STATIC ${core} ${sources} src/chr_shelf.c host/chr_pack_host.c)
target_include_directories(xiaotai_theme_character PUBLIC include PRIVATE src host)
target_link_libraries(xiaotai_theme_character PUBLIC ${requires} PRIVATE m)
target_compile_definitions(xiaotai_theme_character PRIVATE
    XIAOTAI_CHARACTER_PACK="${CMAKE_CURRENT_SOURCE_DIR}/assets/character.pack")
if(TARGET lvgl::lvgl)
    target_sources(xiaotai_theme_character PRIVATE src/chr_actor.c)
    target_link_libraries(xiaotai_theme_character PRIVATE lvgl::lvgl)
endif()
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
