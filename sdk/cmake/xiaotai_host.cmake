# SPDX-License-Identifier: MIT
# Host build helpers shared by every SDK component and application.
#
# The host project (tests/sdk/CMakeLists.txt) includes this file once; the
# functions are then visible in every component's CMakeLists.txt, in the branch
# after `if(ESP_PLATFORM) ... return() endif()`. Nothing here runs in an ESP-IDF
# build.
#
#   xiaotai_host_require(<component>...)
#       Adds each named component to the build unless its target already exists.
#       A component is a directory <root>/<name> with a CMakeLists.txt or an
#       include/ directory, searched under sdk/, sdk/modules/, sdk/themes/,
#       sdk/parts/ and apps/. Its CMakeLists.txt must define a target named
#       after the directory; a directory with only include/ becomes an INTERFACE
#       library. Call it before add_library() for everything the component
#       links, so dependencies are pulled in on demand and in any order.
#
#   xiaotai_host_test(<name> SOURCES <file>... [LINK <target>...]
#                     [INCLUDE_ONLY <component>...] [INCLUDE <dir>...]
#                     [ROLES <role>...] [TIMEOUT <s>] [STACK])
#       Builds one test executable with warnings as errors and the address and
#       undefined-behaviour sanitizers, and registers it with CTest. LINK names
#       components (required on demand) or any other target or library.
#       INCLUDE_ONLY adds the include directories of the named components and of
#       everything they publicly depend on WITHOUT linking them: the test then
#       supplies fakes of their functions. INCLUDE adds plain directories (for
#       example ../src for a component's internal headers).
#       ROLES names verified third-party source trees needed for a test.
#       STACK selects the unsanitized stack-measurement pass.
#
#   xiaotai_host_role_path(<out> <role> <test>...)
#       Resolve a role before creating a third-party test fixture or listing its
#       sources. If skipping is explicitly allowed and the role is missing, the
#       result is empty and each named test is reported as skipped.
#
#   xiaotai_host_components(<out>)
#       Every component found under the search roots, sorted.
#
# Board packages (boards/<vendor>/<board>) all define the component
# xiaotai_board, because a product links exactly one board. The host project
# therefore builds one board per configuration: -DXIAOTAI_BOARD=<vendor>/<board>
# makes the name xiaotai_board resolve to that directory, and from there a
# board is a component like any other (host branch, test/ directory, tests
# against the parts library's host fakes). Boards are never part of "every
# component found"; tests/sdk/run.py builds them one after another.
include_guard(GLOBAL)

if(NOT DEFINED XIAOTAI_ROOT)
    get_filename_component(XIAOTAI_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
endif()
set(XIAOTAI_HOST_COMPONENT_ROOTS sdk sdk/modules sdk/themes sdk/parts apps)
set(_XIAOTAI_HOST_ROLE_SCRIPT "${CMAKE_CURRENT_LIST_DIR}/../../tests/sdk/host_roles.py")
set(_XIAOTAI_HOST_STACK_DIR "${CMAKE_CURRENT_LIST_DIR}/../../tests/sdk/support")

# Applied to every component by the host project and to every test here, so the
# sanitizers see the code under test and not only the test's own sources.
option(XIAOTAI_HOST_SANITIZE "Instrument all host component code with ASan and UBSan" ON)
set(XIAOTAI_HOST_COMPILE_OPTIONS -Wall -Wextra -Werror -fno-omit-frame-pointer)
set(XIAOTAI_HOST_LINK_OPTIONS "")
if(XIAOTAI_HOST_SANITIZE)
    list(APPEND XIAOTAI_HOST_COMPILE_OPTIONS -fsanitize=address,undefined)
    list(APPEND XIAOTAI_HOST_LINK_OPTIONS -fsanitize=address,undefined)
endif()

# A component without a host branch would otherwise stop with "unknown command".
if(NOT ESP_PLATFORM AND NOT COMMAND idf_component_register)
    function(idf_component_register)
        message(FATAL_ERROR
            "${CMAKE_CURRENT_SOURCE_DIR}: idf_component_register() reached in the host build. "
            "Put it inside `if(ESP_PLATFORM) ... return() endif()` and define the host target "
            "(named after the directory) below it; the SDK architecture document under docs/ "
            "describes the component conventions.")
    endfunction()
endif()

# Directory of the selected board package, or empty when none is selected.
function(xiaotai_host_board_dir out)
    set(${out} "" PARENT_SCOPE)
    if(NOT XIAOTAI_BOARD)
        return()
    endif()
    if(NOT XIAOTAI_BOARD MATCHES "^[a-z0-9][a-z0-9_.-]*/[a-z0-9][a-z0-9_.-]*$")
        message(FATAL_ERROR "XIAOTAI_BOARD must be <vendor>/<board> (a directory under boards/), not ${XIAOTAI_BOARD}")
    endif()
    set(dir "${XIAOTAI_ROOT}/boards/${XIAOTAI_BOARD}")
    if(NOT EXISTS "${dir}/CMakeLists.txt")
        message(FATAL_ERROR "No board package ${XIAOTAI_BOARD}: ${dir}/CMakeLists.txt does not exist")
    endif()
    set(${out} "${dir}" PARENT_SCOPE)
endfunction()

# Directory of a component, or empty when no search root has it.
function(xiaotai_host_component_dir out component)
    if(component STREQUAL "xiaotai_board")
        xiaotai_host_board_dir(dir)
        set(${out} "${dir}" PARENT_SCOPE)
        return()
    endif()
    set(found "")
    foreach(root IN LISTS XIAOTAI_HOST_COMPONENT_ROOTS)
        set(candidate "${XIAOTAI_ROOT}/${root}/${component}")
        if(EXISTS "${candidate}/CMakeLists.txt" OR IS_DIRECTORY "${candidate}/include")
            list(APPEND found "${candidate}")
        endif()
    endforeach()
    list(LENGTH found count)
    if(count GREATER 1)
        message(FATAL_ERROR "Component name ${component} is used by more than one directory: ${found}")
    endif()
    set(${out} "${found}" PARENT_SCOPE)
endfunction()

function(xiaotai_host_components out)
    set(names "")
    foreach(root IN LISTS XIAOTAI_HOST_COMPONENT_ROOTS)
        file(GLOB children LIST_DIRECTORIES true RELATIVE "${XIAOTAI_ROOT}/${root}" "${XIAOTAI_ROOT}/${root}/*")
        foreach(child IN LISTS children)
            set(dir "${XIAOTAI_ROOT}/${root}/${child}")
            if(EXISTS "${dir}/CMakeLists.txt" OR IS_DIRECTORY "${dir}/include")
                if(child IN_LIST names)
                    message(FATAL_ERROR "Component name ${child} is used by more than one directory")
                endif()
                list(APPEND names "${child}")
            endif()
        endforeach()
    endforeach()
    list(SORT names)
    set(${out} "${names}" PARENT_SCOPE)
endfunction()

# One component, in its own function so the per-component test switch does not
# leak into the caller's scope.
function(_xiaotai_host_add component dir)
    # Tests of a component that is only a dependency are optional; the switch is
    # recomputed from the cache value because this call may be nested inside a
    # dependency whose tests are off.
    set(tests OFF)
    if(DEFINED CACHE{XIAOTAI_HOST_TESTS})
        set(tests "$CACHE{XIAOTAI_HOST_TESTS}")
        if(DEFINED XIAOTAI_HOST_SELECTED AND NOT XIAOTAI_HOST_DEPENDENCY_TESTS
           AND NOT component IN_LIST XIAOTAI_HOST_SELECTED)
            set(tests OFF)
        endif()
        set(XIAOTAI_HOST_TESTS "${tests}")
    elseif(XIAOTAI_HOST_TESTS)
        set(tests ON)
    endif()

    if(EXISTS "${dir}/CMakeLists.txt")
        add_subdirectory("${dir}" "${CMAKE_BINARY_DIR}/components/${component}")
        if(NOT TARGET "${component}")
            message(FATAL_ERROR
                "${dir}/CMakeLists.txt defines no host target named ${component}. "
                "A component with nothing to compile on the host declares "
                "add_library(${component} INTERFACE) and its include directory.")
        endif()
        # The board registers itself through a function the hardware abstraction
        # calls (SelectedBoard()), so nothing outside the board references its
        # objects: link the whole archive, as the ESP-IDF build registers it
        # WHOLE_ARCHIVE.
        get_target_property(type "${component}" TYPE)
        if(XIAOTAI_HOST_PRODUCT_OPTIONS AND type STREQUAL "STATIC_LIBRARY")
            target_compile_options("${component}" PRIVATE -include "${XIAOTAI_HOST_PRODUCT_OPTIONS}")
        endif()
        if(component STREQUAL "xiaotai_board" AND type STREQUAL "STATIC_LIBRARY")
            target_link_options("${component}" INTERFACE
                "LINKER:--whole-archive" "$<TARGET_FILE:${component}>" "LINKER:--no-whole-archive")
        endif()
        if(tests AND EXISTS "${dir}/test/CMakeLists.txt")
            get_property(children DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
            if(NOT "${dir}/test" IN_LIST children)
                message(FATAL_ERROR
                    "${dir} has test/CMakeLists.txt but never adds it. End its CMakeLists.txt with: "
                    "if(XIAOTAI_HOST_TESTS) add_subdirectory(test) endif()")
            endif()
        endif()
    else()
        add_library("${component}" INTERFACE)
        target_include_directories("${component}" INTERFACE "${dir}/include")
    endif()
endfunction()

function(xiaotai_host_require)
    foreach(component IN LISTS ARGN)
        if(TARGET "${component}")
            continue()
        endif()
        get_property(visiting GLOBAL PROPERTY XIAOTAI_HOST_VISITING)
        if(component IN_LIST visiting)
            string(REPLACE ";" " -> " chain "${visiting}")
            message(FATAL_ERROR "Component dependency cycle: ${chain} -> ${component}")
        endif()
        xiaotai_host_component_dir(dir "${component}")
        if(NOT dir AND component STREQUAL "xiaotai_board")
            message(FATAL_ERROR
                "xiaotai_board (required from ${CMAKE_CURRENT_SOURCE_DIR}) needs a board: configure with "
                "-DXIAOTAI_BOARD=<vendor>/<board>, or run tests/sdk/run.py --board <vendor>/<board>")
        endif()
        if(NOT dir)
            string(REPLACE ";" "/, " roots "${XIAOTAI_HOST_COMPONENT_ROOTS}")
            message(FATAL_ERROR
                "No component ${component} (required from ${CMAKE_CURRENT_SOURCE_DIR}): "
                "looked for ${component}/CMakeLists.txt or ${component}/include under ${roots}/ in ${XIAOTAI_ROOT}")
        endif()
        set_property(GLOBAL APPEND PROPERTY XIAOTAI_HOST_VISITING "${component}")
        _xiaotai_host_add("${component}" "${dir}")
        get_property(visiting GLOBAL PROPERTY XIAOTAI_HOST_VISITING)
        list(REMOVE_ITEM visiting "${component}")
        set_property(GLOBAL PROPERTY XIAOTAI_HOST_VISITING "${visiting}")
        # Read by the all-headers check: what was built, and where it lives.
        set_property(GLOBAL APPEND PROPERTY XIAOTAI_HOST_COMPONENTS "${component}")
        set_property(GLOBAL PROPERTY "XIAOTAI_HOST_DIR_${component}" "${dir}")
    endforeach()
endfunction()

# Include directories a target publishes, with those of its public dependencies.
function(_xiaotai_host_interface_includes out)
    set(queue ${ARGN})
    set(seen "")
    set(dirs "")
    while(queue)
        list(POP_FRONT queue item)
        if(item IN_LIST seen OR NOT TARGET "${item}")
            continue()
        endif()
        list(APPEND seen "${item}")
        get_target_property(include_dirs "${item}" INTERFACE_INCLUDE_DIRECTORIES)
        if(include_dirs)
            list(APPEND dirs ${include_dirs})
        endif()
        get_target_property(libraries "${item}" INTERFACE_LINK_LIBRARIES)
        if(libraries)
            list(APPEND queue ${libraries})
        endif()
    endwhile()
    list(REMOVE_DUPLICATES dirs)
    set(${out} "${dirs}" PARENT_SCOPE)
endfunction()

function(xiaotai_host_role_path out role)
    if(NOT role MATCHES "^[a-z][a-z0-9-]*$" OR NOT ARGN)
        message(FATAL_ERROR "xiaotai_host_role_path expects a role and the affected test names")
    endif()
    set(key "XIAOTAI_HOST_VERIFIED_${role}")
    get_property(resolved GLOBAL PROPERTY "${key}" SET)
    if(resolved)
        get_property(path GLOBAL PROPERTY "${key}")
    elseif(DEFINED ${key})
        set(path "${${key}}")
    else()
        if(NOT XIAOTAI_HOST_PYTHON)
            find_package(Python3 REQUIRED COMPONENTS Interpreter)
            set(XIAOTAI_HOST_PYTHON "${Python3_EXECUTABLE}")
        endif()
        execute_process(COMMAND "${XIAOTAI_HOST_PYTHON}" -B "${_XIAOTAI_HOST_ROLE_SCRIPT}" "${role}"
                        RESULT_VARIABLE status OUTPUT_VARIABLE path ERROR_VARIABLE reason
                        OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_STRIP_TRAILING_WHITESPACE)
        if(NOT status STREQUAL "0" AND NOT (XIAOTAI_HOST_ALLOW_MISSING AND status STREQUAL "3"))
            message(FATAL_ERROR "Host role ${role} required by ${ARGN}: ${reason}")
        endif()
    endif()
    set_property(GLOBAL PROPERTY "${key}" "${path}")
    if(NOT path)
        if(NOT XIAOTAI_HOST_ALLOW_MISSING)
            message(FATAL_ERROR "Host role ${role} required by ${ARGN} is missing")
        endif()
        foreach(test IN LISTS ARGN)
            file(APPEND "${CMAKE_BINARY_DIR}/host-skipped.txt" "${test} (needs ${role})\n")
        endforeach()
    endif()
    set(${out} "${path}" PARENT_SCOPE)
endfunction()

function(xiaotai_host_test name)
    cmake_parse_arguments(PARSE_ARGV 1 arg "STACK" "TIMEOUT" "SOURCES;LINK;INCLUDE_ONLY;INCLUDE;ROLES")
    if(arg_UNPARSED_ARGUMENTS OR NOT arg_SOURCES)
        message(FATAL_ERROR
            "xiaotai_host_test(${name}): expected SOURCES <file>... [LINK <target>...] "
            "[INCLUDE_ONLY <component>...] [INCLUDE <dir>...] [ROLES <role>...] [TIMEOUT <seconds>] [STACK]; got ${ARGN}")
    endif()
    if(TARGET "${name}")
        message(FATAL_ERROR
            "xiaotai_host_test(${name}) in ${CMAKE_CURRENT_SOURCE_DIR}: the name is already taken. "
            "Test names are global; start them with the component's short name.")
    endif()
    if(NOT arg_TIMEOUT)
        set(arg_TIMEOUT 120)
    endif()
    foreach(role IN LISTS arg_ROLES)
        xiaotai_host_role_path(path "${role}" "${name}")
        if(NOT path)
            return()
        endif()
    endforeach()

    if(arg_STACK)
        file(APPEND "${CMAKE_BINARY_DIR}/host-stack-tests.txt" "${name}\n")
    endif()
    if((XIAOTAI_HOST_SANITIZE AND arg_STACK) OR (NOT XIAOTAI_HOST_SANITIZE AND NOT arg_STACK))
        # Keep a target for existing per-test compile options without building
        # its sources or registering it in this pass.
        add_executable("${name}" EXCLUDE_FROM_ALL ${arg_SOURCES})
        return()
    endif()

    add_executable("${name}" ${arg_SOURCES})
    if(arg_STACK)
        target_sources("${name}" PRIVATE "${_XIAOTAI_HOST_STACK_DIR}/host_stack.c")
        target_include_directories("${name}" PRIVATE "${_XIAOTAI_HOST_STACK_DIR}")
    endif()
    # Repeats the host project's directory options on purpose: the test keeps its
    # checks when a component is built from another top-level project.
    target_compile_options("${name}" PRIVATE ${XIAOTAI_HOST_COMPILE_OPTIONS})
    target_link_options("${name}" PRIVATE ${XIAOTAI_HOST_LINK_OPTIONS})

    get_property(visiting GLOBAL PROPERTY XIAOTAI_HOST_VISITING)
    foreach(item IN LISTS arg_LINK)
        # A component still being configured gets its target before generation;
        # anything that is not a component (m, Threads::Threads) passes through.
        if(NOT TARGET "${item}" AND NOT item IN_LIST visiting)
            xiaotai_host_component_dir(dir "${item}")
            if(dir)
                xiaotai_host_require("${item}")
            endif()
        endif()
    endforeach()
    if(arg_LINK)
        target_link_libraries("${name}" PRIVATE ${arg_LINK})
    endif()

    if(arg_INCLUDE_ONLY)
        xiaotai_host_require(${arg_INCLUDE_ONLY})
        _xiaotai_host_interface_includes(dirs ${arg_INCLUDE_ONLY})
        target_include_directories("${name}" PRIVATE ${dirs})
    endif()
    if(arg_INCLUDE)
        target_include_directories("${name}" PRIVATE ${arg_INCLUDE})
    endif()

    # Label = owning component, so `ctest -L <component>` selects its tests.
    get_filename_component(label "${CMAKE_CURRENT_SOURCE_DIR}" NAME)
    if(label STREQUAL "test")
        get_filename_component(parent "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
        get_filename_component(label "${parent}" NAME)
    endif()
    if(arg_STACK)
        list(APPEND label stack)
    endif()
    add_test(NAME "${name}" COMMAND "${name}")
    set_tests_properties("${name}" PROPERTIES TIMEOUT "${arg_TIMEOUT}" LABELS "${label}")
endfunction()
