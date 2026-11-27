cmake_minimum_required(VERSION 3.22)

include("${XIAOTAI_GENERATED_DIR}/selection.cmake")
set(COMPONENTS main)
set(XIAOTAI_PORT_V3_PRODUCT ON CACHE INTERNAL "Use the SDK-native ESP port")
foreach(selected IN LISTS XIAOTAI_SELECTED_COMPONENT_DIRS)
    list(APPEND EXTRA_COMPONENT_DIRS "${XIAOTAI_REPO}/${selected}")
endforeach()
list(APPEND EXTRA_COMPONENT_DIRS "${XIAOTAI_REPO}/components/tirtc_sdk")
include("${XIAOTAI_REPO}/sdk/cmake/xiaotai_board.cmake")
xiaotai_board_idf_component("${XIAOTAI_REPO}/boards/${XIAOTAI_BOARD}" board_component)
list(APPEND EXTRA_COMPONENT_DIRS "${board_component}")

include("$ENV{IDF_PATH}/tools/cmake/version.cmake")
set(ENV{ESP_IDF_VERSION} "${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}")
include("$ENV{IDF_PATH}/tools/cmake/project.cmake")
foreach(property IN ITEMS XIAOTAI_SELECTED XIAOTAI_SELECTED_COMPONENTS XIAOTAI_GENERATED_DIR
                          XIAOTAI_TIRTC_PACKAGE XIAOTAI_PORT_V3_PRODUCT DEPENDENCIES_LOCK)
    idf_build_set_property(${property} "${${property}}")
endforeach()
set(PROJECT_VER "0.5.0")
string(REPLACE "-" "_" project_name "${XIAOTAI_PRODUCT_NAME}")
project("${project_name}")

# Data a unit carries behind the application image, in the same file (scripts/image_trailer.py).
# It is attached as soon as the image exists, before anything checks, flashes or merges it.
idf_build_get_property(image_trailer XIAOTAI_IMAGE_TRAILER)
if(image_trailer)
    idf_build_get_property(trailer_python PYTHON)
    set(trailer_script "${XIAOTAI_REPO}/scripts/image_trailer.py")
    add_custom_target(xiaotai_image_trailer
        COMMAND "${trailer_python}" "${trailer_script}" attach "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.bin"
                "${image_trailer}" --record "${CMAKE_BINARY_DIR}/image_trailer.json"
        DEPENDS "${CMAKE_BINARY_DIR}/.bin_timestamp" "${image_trailer}" "${trailer_script}"
        VERBATIM)
    add_dependencies(gen_project_binary xiaotai_image_trailer)
endif()

idf_build_get_property(idle_stack_archive XIAOTAI_TIRTC_IDLE_STACK_ARCHIVE)
if(idle_stack_archive)
    idf_build_get_property(idf_python PYTHON)
    idf_build_get_property(session_stack_policy XIAOTAI_TIRTC_SESSION_STACK_POLICY)
    if(session_stack_policy)
        set(session_stack_flag --session)
    endif()
    set(check_idle_stack "${XIAOTAI_REPO}/sdk/xiaotai_transport/esp-idf/verify_idle_stack_link.py")
    set_property(TARGET "${CMAKE_PROJECT_NAME}.elf" APPEND PROPERTY LINK_DEPENDS
                 "${check_idle_stack}" "${idle_stack_archive}")
    # Reject an unexpected linked call path before making a firmware binary.
    add_custom_command(TARGET "${CMAKE_PROJECT_NAME}.elf" POST_BUILD
        COMMAND "${idf_python}" "${check_idle_stack}" "${CMAKE_NM}" "${CMAKE_OBJDUMP}"
                "${idle_stack_archive}" "$<TARGET_FILE:${CMAKE_PROJECT_NAME}.elf>"
                "${CMAKE_BINARY_DIR}/${CMAKE_PROJECT_NAME}.map" ${session_stack_flag} VERBATIM)
endif()
