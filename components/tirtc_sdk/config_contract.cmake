# SPDX-License-Identifier: MIT
# Settings the archive was compiled against. Each table lists symbols that must
# have exactly the recorded value in this build: the C library (libc_config;
# newlib and picolibc objects link together but disagree on errno, ctype and
# FILE), the ESP32-S3 PSRAM atomic workaround (atomic_config), and external RAM
# for an archive that allocates its buffers or task stacks from it
# (memory_config).
function(tirtc_validate_config_tables contract)
    foreach(table IN ITEMS libc_config atomic_config memory_config)
        string(JSON table_count ERROR_VARIABLE table_missing LENGTH "${contract}" ${table})
        if(table_missing OR table_count EQUAL 0)
            continue()
        endif()
        math(EXPR table_last "${table_count}-1")
        foreach(index RANGE ${table_last})
            string(JSON symbol MEMBER "${contract}" ${table} ${index})
            string(JSON expected GET "${contract}" ${table} ${symbol})
            if(NOT "${${symbol}}" STREQUAL "${expected}")
                message(FATAL_ERROR "TiRTC ${table} mismatch: ${symbol} requires ${expected}")
            endif()
        endforeach()
    endforeach()
endfunction()
