# SPDX-License-Identifier: MIT
# XiaoTai application "doorbell" (xiaotai/apps/doorbell.h; manifest name
# "doorbell"): the door station of a household's doorbell on a device without
# a display. One key, lights, and the door application's call messages.
#   src/doorbell.c   key, the round of indoor stations, looks, door messages, sounds and lights,
#                    and the spoken binding code
# The optional video_call module only adds the camera to the calls it dials;
# the source asks for it at run time (xiaotai_module_present).
set(requires xiaotai_port xiaotai_framework xiaotai_mod_call xiaotai_mod_contacts xiaotai_mod_binding
             xiaotai_feedback xiaotai_interaction xiaotai_sounds xiaotai_eventlog)
set(sources src/doorbell.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" REQUIRES ${requires})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_app_doorbell STATIC ${sources})
target_include_directories(xiaotai_app_doorbell PUBLIC include)
target_link_libraries(xiaotai_app_doorbell PUBLIC ${requires})
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
