# SPDX-License-Identifier: MIT
# XiaoTai stream module: the web real-time stream (xiaotai/modules/stream.h).
# A viewer dials the device; this module owns the inbound listener, decides
# when it admits, runs the audio session and writes the "stream" scene of the
# device profile. Video is a session extension (xiaotai_mod_stream_video) that
# attaches to the stream activity and contributes the link's video part and
# the scene's video members (xiaotai/framework.h, xiaotai_activity_contributions);
# this module never touches a camera and offers the extension no API.
#
set(sources src/stream.c src/stream_profile.c src/stream_at.c)
set(requires xiaotai_port xiaotai_framework xiaotai_mod_binding xiaotai_platform xiaotai_transport
             xiaotai_media xiaotai_eventlog xiaotai_at)
if(ESP_PLATFORM)
    idf_build_get_property(selected XIAOTAI_SELECTED)
elseif(XIAOTAI_HOST_PRODUCT_SELECTION)
    set(selected ${XIAOTAI_SELECTED})
endif()
if("contacts" IN_LIST selected)
    list(APPEND requires xiaotai_mod_contacts)
endif()
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" REQUIRES ${requires})
    if("contacts" IN_LIST selected)
        target_compile_definitions(${COMPONENT_LIB} PRIVATE XIAOTAI_STREAM_HAS_CONTACTS=1)
    endif()
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_mod_stream STATIC ${sources})
target_include_directories(xiaotai_mod_stream PUBLIC include)
target_link_libraries(xiaotai_mod_stream PUBLIC ${requires})
if("contacts" IN_LIST selected)
    target_compile_definitions(xiaotai_mod_stream PRIVATE XIAOTAI_STREAM_HAS_CONTACTS=1)
endif()
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
