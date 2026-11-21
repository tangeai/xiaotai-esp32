# SPDX-License-Identifier: MIT
# XiaoTai recognition module: sound recognition
# (xiaotai/modules/recognition.h). It records clean microphone audio, submits
# it under a request id that is stored before it is sent, holds the task's
# result, plays its speech once when asked (or by itself when the product
# configures autoplay) and carries the user's decisions. The platform
# core carries every exchange (requests and streamed transfers), the media
# engine the recording and the playback, the storage port the record.
#
# Planned portable sources (both branches share the list; no platform sources):
#   src/recognition.c            owner state machine, status, events, intents
#   src/recognition_admission.c  policy and size rules (pure functions)
#   src/recognition_policy.c     policy request and parsing
#   src/recognition_upload.c     multipart submission as a streamed transfer
#   src/recognition_task.c       task query, notification and result validation
#   src/recognition_audio.c      speech download
#   src/recognition_playback.c   playback order and consumption states
#   src/recognition_decision.c   resume, dismiss, archive
#   src/recognition_result.c     the reference-counted result view
#   src/recognition_journal.c    the stored record: formats 1..4, scope, worker
#   src/recognition_at.c         AT+RECOGNITION, diagnostics provider
# Replies, notifications and the stored record are read with xiaotai/json.h
# (count first, then a token table from BULK) and the record is written with
# its writer: no tokenizer of the module's own.
# The environment switch reaches the module through a framework quiesce guard
# (reason XIAOTAI_QUIESCE_ENVIRONMENT, xiaotai/framework.h), so the
# environment module is neither required nor included: a product may leave it
# out.
set(requires xiaotai_port xiaotai_json xiaotai_framework xiaotai_mod_binding xiaotai_mod_recognition_config xiaotai_platform
             xiaotai_identity xiaotai_media xiaotai_sounds xiaotai_eventlog xiaotai_at)
set(sources
    src/recognition.c
    src/recognition_journal.c
    src/recognition_admission.c
    src/recognition_policy.c
    src/recognition_upload.c
    src/recognition_task.c
    src/recognition_audio.c
    src/recognition_playback.c
    src/recognition_result.c
    src/recognition_decision.c
    src/recognition_at.c
)
if(ESP_PLATFORM)
    idf_component_register(SRCS ${sources} INCLUDE_DIRS "include" REQUIRES ${requires})
    return()
endif()

# Host build (tests/sdk; helpers in sdk/cmake/xiaotai_host.cmake).
xiaotai_host_require(${requires})
add_library(xiaotai_mod_recognition STATIC ${sources})
target_include_directories(xiaotai_mod_recognition PUBLIC include)
target_link_libraries(xiaotai_mod_recognition PUBLIC ${requires})
if(XIAOTAI_HOST_TESTS)
    target_compile_definitions(xiaotai_mod_recognition PRIVATE XIAOTAI_RECOGNITION_JOURNAL_TEST)
    add_subdirectory(test)
endif()
