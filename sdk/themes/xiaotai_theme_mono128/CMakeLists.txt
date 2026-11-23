# SPDX-License-Identifier: MIT
# Renders whole frames without a drawing library.
set(sources src/mono_canvas.c src/mono_glyph5.c src/mono_theme.c src/mono_pages.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include"
                           REQUIRES xiaotai_ui)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_ui)
add_library(xiaotai_theme_mono128 STATIC ${sources})
target_include_directories(xiaotai_theme_mono128 PUBLIC include)
target_link_libraries(xiaotai_theme_mono128 PUBLIC xiaotai_ui)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
