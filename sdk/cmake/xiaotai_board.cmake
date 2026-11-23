# SPDX-License-Identifier: MIT
# Board-component build glue, shared by IDF and the standalone host board tests.
include_guard(GLOBAL)

set_property(GLOBAL PROPERTY XIAOTAI_BOARD_ROOT "${CMAKE_CURRENT_LIST_DIR}/../..")

# IDF derives the component name from the directory basename. Give the one
# selected board that name in the build tree, without symlinks or source copies.
function(xiaotai_board_idf_component board_dir out)
    get_property(root GLOBAL PROPERTY XIAOTAI_BOARD_ROOT)
    get_filename_component(board_dir "${board_dir}" REALPATH)
    if(NOT EXISTS "${board_dir}/board.json" OR NOT EXISTS "${board_dir}/CMakeLists.txt")
        message(FATAL_ERROR "Not a XiaoTai board package: ${board_dir}")
    endif()
    set(dir "${CMAKE_BINARY_DIR}/board-component/xiaotai_board")
    configure_file("${root}/sdk/cmake/xiaotai_board_idf.cmake.in" "${dir}/CMakeLists.txt" @ONLY)
    set(${out} "${dir}" PARENT_SCOPE)
endfunction()

function(xiaotai_board_header manifest out)
    get_property(root GLOBAL PROPERTY XIAOTAI_BOARD_ROOT)
    if(ESP_PLATFORM)
        idf_build_get_property(python PYTHON)
    else()
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        set(python "${Python3_EXECUTABLE}")
    endif()
    set(selector "${root}/scripts/sdk_selection.py")
    # Register the same spelling as the HAL tests so Ninja sees one configure input.
    get_filename_component(needs_json "${root}/sdk/xiaotai_hal/needs.json" REALPATH)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
                 "${manifest}" "${selector}" "${needs_json}")
    set(dir "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${dir}")
    execute_process(COMMAND "${python}" -B "${selector}" --board-header "${manifest}"
                    --out "${dir}/xiaotai_board_data.hpp"
                    RESULT_VARIABLE code ERROR_VARIABLE error)
    if(NOT code STREQUAL "0")
        message(FATAL_ERROR "Cannot generate board constants: ${error}")
    endif()
    set(${out} "${dir}" PARENT_SCOPE)
endfunction()
