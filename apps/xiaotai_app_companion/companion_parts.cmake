# SPDX-License-Identifier: MIT

# Generate the private table in manifest order. Empty is a valid product.
function(xiaotai_companion_parts out directory output available selected)
    set(declarations "")
    set(entries "")
    set(sources "")
    foreach(component IN LISTS available)
        if(NOT component IN_LIST selected)
            continue()
        endif()
        string(REGEX REPLACE "^xiaotai_(mod_)?" "" name "${component}")
        set(source "${directory}/with_${name}.c")
        # Later work packages add the functional adapters. Do not advertise
        # selected modules that have no companion adapter implemented yet.
        if(EXISTS "${source}")
            set(symbol "xiaotai_app_companion_with_${name}")
            string(APPEND declarations "extern const xiaotai_app_companion_part_t ${symbol};\n")
            string(APPEND entries "    &${symbol},\n")
            list(APPEND sources "${source}")
        endif()
    endforeach()
    file(GENERATE OUTPUT "${output}" CONTENT
         "#include \"companion_internal.h\"\n${declarations}\nconst xiaotai_app_companion_part_t *const xiaotai_app_companion_parts[] = {\n${entries}    NULL,\n};\n")
    set_source_files_properties("${output}" PROPERTIES GENERATED TRUE)
    set(${out} ${sources} "${output}" PARENT_SCOPE)
endfunction()
