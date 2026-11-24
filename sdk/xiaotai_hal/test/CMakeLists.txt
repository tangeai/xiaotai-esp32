# SPDX-License-Identifier: MIT
# Host tests of the hardware abstraction, with fake boards. xiaotai_hal_init()
# runs once per process, so each bring-up outcome is a test of its own:
# hal_board after a successful bring-up, hal_init_failure after a failed one.
#
# Both read needs_rules.h, generated here from ../needs.json: the rules and
# the vocabulary as C++ tables, so that hal_board checks the derivation in
# src/hal.cc against the file itself, kind by kind, and a rule added to the
# file is tested without editing the test. A 'where' field the generator does
# not know stops the configuration: the test must learn it first.
get_filename_component(needs_json "${CMAKE_CURRENT_SOURCE_DIR}/../needs.json" REALPATH)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${needs_json}")
file(READ "${needs_json}" needs_text)

# "{"a", "b", nullptr}" from a JSON array of strings.
function(_hal_test_string_list out array)
    string(JSON count LENGTH "${array}")
    set(items "")
    if(count GREATER 0)
        math(EXPR last "${count} - 1")
        foreach(i RANGE ${last})
            string(JSON item GET "${array}" ${i})
            string(APPEND items "\"${item}\", ")
        endforeach()
    endif()
    set(${out} "{${items}nullptr}" PARENT_SCOPE)
endfunction()

set(lists "")
set(rules "")
string(JSON need_count LENGTH "${needs_text}" needs)
math(EXPR need_last "${need_count} - 1")
foreach(i RANGE ${need_last})
    string(JSON rule GET "${needs_text}" needs ${i})
    string(JSON name GET "${rule}" name)
    string(JSON bit GET "${rule}" bit)
    string(JSON part ERROR_VARIABLE no_part GET "${rule}" part)
    string(JSON memory ERROR_VARIABLE no_memory GET "${rule}" memory)
    set(part_text "nullptr")
    set(memory_text "nullptr")
    if(NOT no_part)
        set(part_text "\"${part}\"")
    endif()
    if(NOT no_memory)
        set(memory_text "\"${memory}\"")
    endif()
    set(fields kind role encoding)
    foreach(field IN LISTS fields)
        set(${field}_text "nullptr")
    endforeach()
    set(touch_text "-1")
    string(JSON where ERROR_VARIABLE no_where GET "${rule}" where)
    if(NOT no_where)
        string(JSON where_count LENGTH "${where}")
        math(EXPR where_last "${where_count} - 1")
        foreach(w RANGE ${where_last})
            string(JSON key MEMBER "${where}" ${w})
            string(JSON values GET "${where}" ${key})
            if(key IN_LIST fields)
                _hal_test_string_list(items "${values}")
                string(APPEND lists "inline constexpr const char* k${name}_${key}[] = ${items};\n")
                set(${key}_text "k${name}_${key}")
            elseif(key STREQUAL "touch")
                string(JSON value_count LENGTH "${values}")
                string(JSON value GET "${values}" 0)
                if(NOT value_count EQUAL 1)
                    message(FATAL_ERROR "${needs_json}: need ${name}: hal tests take one touch value")
                endif()
                set(touch_text "0")
                if(value)
                    set(touch_text "1")
                endif()
            else()
                message(FATAL_ERROR "${needs_json}: need ${name}: 'where' field ${key} is unknown to "
                                    "${CMAKE_CURRENT_LIST_FILE}; teach the hal tests to build parts for it")
            endif()
        endforeach()
    endif()
    string(APPEND rules "    {\"${name}\", ${bit}, ${part_text}, ${memory_text}, ${kind_text}, ${role_text}, "
                        "${encoding_text}, ${touch_text}},\n")
endforeach()

set(vocabulary "")
string(JSON vocabulary_count LENGTH "${needs_text}" vocabulary)
math(EXPR vocabulary_last "${vocabulary_count} - 1")
foreach(v RANGE ${vocabulary_last})
    string(JSON field MEMBER "${needs_text}" vocabulary ${v})
    string(JSON map GET "${needs_text}" vocabulary ${field})
    string(REPLACE "." "_" identifier "${field}")
    string(APPEND vocabulary "inline constexpr Spelling k${identifier}[] = {\n")
    string(JSON map_count LENGTH "${map}")
    math(EXPR map_last "${map_count} - 1")
    foreach(m RANGE ${map_last})
        string(JSON spelling MEMBER "${map}" ${m})
        string(JSON enumerator GET "${map}" ${spelling})
        string(APPEND vocabulary "    {\"${spelling}\", static_cast<uint32_t>(${enumerator})},\n")
    endforeach()
    string(APPEND vocabulary "    {nullptr, 0},\n};\n")
endforeach()

file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/needs_rules.h" CONTENT
"// SPDX-License-Identifier: MIT
// Generated from sdk/xiaotai_hal/needs.json by sdk/xiaotai_hal/test/CMakeLists.txt.
#pragma once
#include <cstdint>
#include \"xiaotai/hal.h\"

namespace needs_json {

// One rule: the need's bit is satisfied by a part of class 'part' whose fields
// match every non-null list (kind, encoding: spellings; role: role families),
// and touch when it is not -1; or, for 'memory', by the memory port.
struct Rule {
    const char* name;
    unsigned bit;
    const char* part;
    const char* memory;
    const char* const* kind;
    const char* const* role;
    const char* const* encoding;
    int touch;
};

struct Spelling {
    const char* text;
    uint32_t value;
};

@lists@
inline constexpr Rule kRules[] = {
@rules@};

@vocabulary@
}  // namespace needs_json
" @ONLY)

xiaotai_host_test(hal_board SOURCES hal_test.cc LINK xiaotai_hal INCLUDE "${CMAKE_CURRENT_BINARY_DIR}")
xiaotai_host_test(hal_init_failure SOURCES hal_init_failure_test.cc LINK xiaotai_hal
                  INCLUDE "${CMAKE_CURRENT_BINARY_DIR}")
xiaotai_host_test(hal_fake_parts SOURCES fake_parts_test.cc fake_parts.cc LINK xiaotai_port
                  INCLUDE_ONLY xiaotai_hal INCLUDE include)
