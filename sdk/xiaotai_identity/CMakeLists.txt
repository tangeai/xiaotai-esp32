# SPDX-License-Identifier: MIT
# XiaoTai identity and preferences: the device identity in the storage layout
# deployed firmware uses, the binding generation, and the place registered
# settings are kept. Portable: the port layer, and the runtime framework for
# the setting store its engine init installs and the owner-task timer that
# schedules preference writes (the framework does not link this component).
# Saves, erases and preference writes are asynchronous jobs of the storage
# port's worker and create no task; only a reload runs on a short-lived task
# of its own (xiaotai/identity.h). Part of every product. Nothing differs
# between ESP-IDF and the host, so there are no esp-idf/ or host/ directories.
#
set(sources src/identity.c src/identity_layout.c src/fingerprint.c src/identity_jobs.c
            src/preferences.c src/preferences_record.c src/setting_store.c)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" PRIV_INCLUDE_DIRS "src"
                           REQUIRES xiaotai_port xiaotai_framework)
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(xiaotai_port xiaotai_framework)
add_library(xiaotai_identity STATIC ${sources})
target_include_directories(xiaotai_identity PUBLIC include PRIVATE src)
target_link_libraries(xiaotai_identity PUBLIC xiaotai_port xiaotai_framework)
if(XIAOTAI_HOST_TESTS)
    add_subdirectory(test)
endif()
