# SPDX-License-Identifier: MIT
# XiaoTai stream video module: video on the web real-time stream
# (xiaotai/modules/stream_video.h). It attaches to the stream activity as a
# session extension, contributing the link's video part (xiaotai/link.h,
# XIAOTAI_LINK_SLOT_VIDEO) and the scene's video members (xiaotai/platform.h,
# XIAOTAI_PLATFORM_SLOT_SCENE), and runs a session of the video engine on the
# stream's link: the camera to the viewer and, where the product shows
# pictures, the viewer's video. It draws nothing itself.
#
set(sources src/stream_video.c src/stream_video_profile.c)
set(requires xiaotai_port xiaotai_framework xiaotai_mod_stream xiaotai_platform xiaotai_video
             xiaotai_transport xiaotai_eventlog xiaotai_at)
if(ESP_PLATFORM)
    idf_build_get_property(selected XIAOTAI_SELECTED)
elseif(XIAOTAI_HOST_PRODUCT_SELECTION)
    set(selected ${XIAOTAI_SELECTED})
else()
    set(selected ui)
endif()
if("ui" IN_LIST selected)
    list(APPEND requires xiaotai_ui)
    list(APPEND sources src/with_ui.c)
    set(with_ui XIAOTAI_STREAM_VIDEO_WITH_UI=1)
endif()
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" REQUIRES ${requires})
    if(with_ui)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE ${with_ui})
    endif()
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_mod_stream_video STATIC ${sources})
target_include_directories(xiaotai_mod_stream_video PUBLIC include)
target_compile_definitions(xiaotai_mod_stream_video PRIVATE ${with_ui})
target_link_libraries(xiaotai_mod_stream_video PUBLIC ${requires})
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
