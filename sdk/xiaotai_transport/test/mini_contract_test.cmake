# SPDX-License-Identifier: MIT
# Replaces legacy: tests/test_native_profiles.py
cmake_minimum_required(VERSION 3.20)
include("${CMAKE_CURRENT_LIST_DIR}/../esp-idf/verify_mini_contract.cmake")

if(DEFINED bad_case)
    xiaotai_tirtc_verify_mini_contract("${contract}" xmini-mini "${header}" enabled)
    return()
endif()

set(locked_header "${CMAKE_CURRENT_LIST_DIR}/../esp-idf/include/tgtrp_internal_diag.h")
xiaotai_tirtc_verify_mini_contract("${mini_contract}" xmini-mini "${locked_header}" enabled)
if(NOT enabled)
    message(FATAL_ERROR "The selected locked Mini package must enable the hook")
endif()
xiaotai_tirtc_verify_mini_contract("${generic_contract}" generic-esp32c3 "absent.h" enabled)
if(enabled)
    message(FATAL_ERROR "A package without the Mini contract must not enable the hook")
endif()

function(expect_failure contract header pattern)
    execute_process(COMMAND "${CMAKE_COMMAND}" -Dbad_case=1 "-Dcontract=${contract}" "-Dheader=${header}"
                    -P "${CMAKE_CURRENT_LIST_FILE}" RESULT_VARIABLE status ERROR_VARIABLE error)
    if(status EQUAL 0 OR NOT error MATCHES "${pattern}")
        message(FATAL_ERROR "Unexpected result for invalid contract input: ${status}\n${error}")
    endif()
endfunction()

set(changed_header "${fixture_dir}/mini-test-changed-header.h")
configure_file("${locked_header}" "${changed_header}" COPYONLY)
file(APPEND "${changed_header}" "\n")
expect_failure("${mini_contract}" "${changed_header}" "SHA-256 mismatch")

set(changed_contract "${fixture_dir}/mini-test-changed-contract.json")
file(READ "${mini_contract}" json)
foreach(field pending_close_contract pending_command_requested_byte_budget diagnostic_version)
    string(JSON changed SET "${json}" xmini_contract ${field} 0)
    file(WRITE "${changed_contract}" "${changed}")
    expect_failure("${changed_contract}" "${locked_header}" "contract mismatch")
endforeach()
expect_failure("${generic_contract}" "${locked_header}" "has no xmini_contract")
