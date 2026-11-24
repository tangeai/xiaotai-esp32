# SPDX-License-Identifier: MIT
xiaotai_host_test(port_kv_nvs_strict SOURCES kv_nvs_strict_test.c ../esp-idf/kv_nvs.c
    INCLUDE ../common kv_nvs_stubs INCLUDE_ONLY xiaotai_port)
target_compile_options(port_kv_nvs_strict PRIVATE -ffunction-sections)
target_link_options(port_kv_nvs_strict PRIVATE "LINKER:--gc-sections")
# Host tests of the port layer: the conformance test is written only against
# xiaotai/port headers; the kv test also drives the host backend's fault hooks;
# the mem test drives the host memory port's counts and failure injection.
xiaotai_host_test(port_conformance SOURCES port_conformance.c LINK xiaotai_port)
xiaotai_host_test(port_sys_idf SOURCES sys_idf_test.c ../esp-idf/sys.c
    INCLUDE sys_idf_stubs INCLUDE_ONLY xiaotai_port)
xiaotai_host_test(port_stack_guard SOURCES stack_guard_test.c LINK xiaotai_port)
target_link_options(port_stack_guard PRIVATE "LINKER:--wrap=xiaotai_mem_has_external")
xiaotai_host_test(port_mem_ledger SOURCES mem_ledger_host_test.c LINK xiaotai_port)
foreach(profile report report_probe report_legacy)
    xiaotai_host_test(port_${profile} SOURCES port_report_test.c ../esp-idf/diag.c ../common/port_report.c
        INCLUDE_ONLY xiaotai_port INCLUDE ../common ../esp-idf mem_stubs usb_stubs report_stubs)
endforeach()
target_compile_definitions(port_report PRIVATE XIAOTAI_PORT_NATIVE_STARTUP=1 CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=1
    CONFIG_XIAOTAI_PORT_USB_MONITOR_FIX=1 CONFIG_IDF_TARGET_ESP32S3=1)
target_compile_definitions(port_report_probe PRIVATE XIAOTAI_PORT_NATIVE_STARTUP=1 CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=1
    CONFIG_XIAOTAI_PORT_USB_MONITOR_FIX=1 CONFIG_XIAOTAI_PORT_USB_TIMING_PROBE=1)
target_compile_definitions(port_report_legacy PRIVATE CONFIG_IDF_TARGET_ESP32S3=1 CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=1
    CONFIG_XIAOTAI_PORT_USB_MONITOR_FIX=1 CONFIG_XIAOTAI_PORT_USB_TIMING_PROBE=1)
xiaotai_host_test(port_digest SOURCES digest_test.c LINK xiaotai_port)
xiaotai_host_test(port_log SOURCES log_test.c LINK xiaotai_port)
xiaotai_host_test(port_http SOURCES http_test.c LINK xiaotai_port)
foreach(tls plain tls dns_demand)
    xiaotai_host_test(port_http_idf_${tls} SOURCES http_idf_test.c ../esp-idf/http.c ../esp-idf/http_dns.c
                      LINK xiaotai_port INCLUDE ../common http_idf_stubs)
    target_link_options(port_http_idf_${tls} PRIVATE "LINKER:--wrap=xiaotai_time_us,--wrap=xiaotai_delay_ms")
endforeach()
target_compile_definitions(port_http_idf_tls PRIVATE CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=1)
target_compile_definitions(port_http_idf_dns_demand PRIVATE
    CONFIG_XIAOTAI_PORT_HTTP_DNS_ON_DEMAND=1 CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=1)
xiaotai_host_test(port_mqtt SOURCES mqtt_test.c LINK xiaotai_port INCLUDE ../common)
target_link_options(port_mqtt PRIVATE "LINKER:--wrap=xiaotai_mem_free")
foreach(tls plain tls)
    xiaotai_host_test(port_mqtt_idf_${tls} SOURCES mqtt_idf_test.c ../esp-idf/mqtt.c
                      LINK xiaotai_port INCLUDE ../common mqtt_idf_stubs)
    target_compile_definitions(port_mqtt_idf_${tls} PRIVATE CONFIG_XIAOTAI_PORT_MQTT_NATIVE_OVERLAY=1)
endforeach()
target_compile_definitions(port_mqtt_idf_tls PRIVATE CONFIG_MQTT_TRANSPORT_SSL=1 CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=1)
foreach(stock IN ITEMS port_mqtt_idf_stock port_mqtt_idf_stock_demand)
    xiaotai_host_test(${stock} SOURCES mqtt_stock_test.c ../esp-idf/mqtt.c
                      LINK xiaotai_port INCLUDE ../common mqtt_idf_stubs)
    target_compile_definitions(${stock} PRIVATE CONFIG_MQTT_TRANSPORT_SSL=1 CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=1)
    target_link_options(${stock} PRIVATE "LINKER:--wrap=xiaotai_task_create")
endforeach()
target_compile_definitions(port_mqtt_idf_stock_demand PRIVATE CONFIG_XIAOTAI_PORT_MQTT_PAYLOAD_ON_DEMAND=1)
xiaotai_host_test(port_console SOURCES console_test.c LINK xiaotai_port)
add_test(NAME port_console_stdio COMMAND port_console stdio)
set_tests_properties(port_console_stdio PROPERTIES LABELS xiaotai_port TIMEOUT 10)
foreach(device uart usb none)
    xiaotai_host_test(port_console_idf_${device} SOURCES console_idf_test.c INCLUDE_ONLY xiaotai_port
                      INCLUDE ../common console_stubs mem_stubs)
endforeach()
target_compile_definitions(port_console_idf_uart PRIVATE CONFIG_ESP_CONSOLE_UART_DEFAULT=1
                           CONFIG_ESP_CONSOLE_UART_NUM=1 CONFIG_ESP_CONSOLE_UART_BAUDRATE=921600)
target_compile_definitions(port_console_idf_usb PRIVATE CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=1)
xiaotai_host_test(port_usb_monitor SOURCES usb_monitor_test.c INCLUDE usb_stubs ../esp-idf/usb_monitor)
foreach(hz 10 100)
    xiaotai_host_test(port_usb_monitor_${hz}hz SOURCES usb_monitor_test.c INCLUDE usb_stubs ../esp-idf/usb_monitor)
    target_compile_definitions(port_usb_monitor_${hz}hz PRIVATE TEST_USB_TICK_HZ=${hz}
                               CONFIG_IDF_TARGET_ESP32C3=0 USB_SERIAL_JTAG_LL_PHY_DEPENDS_ON_BBPLL=1)
endforeach()
xiaotai_host_test(port_clock SOURCES clock_test.c LINK xiaotai_port TIMEOUT 15)
target_link_options(port_clock PRIVATE "LINKER:--wrap=setenv")
xiaotai_host_test(port_clock_ntp SOURCES clock_ntp_test.c LINK xiaotai_port INCLUDE ../common)
xiaotai_host_test(port_clock_idf SOURCES clock_idf_test.c ../esp-idf/clock.c
                  LINK xiaotai_port INCLUDE ../common clock_stubs)
target_link_options(port_clock_idf PRIVATE "LINKER:--wrap=gettimeofday,--wrap=settimeofday,--wrap=adjtime")
xiaotai_host_test(port_kv SOURCES kv_test.c LINK xiaotai_port)
# Compiles the worker's source into the test (its slots and interrupt context
# are observed directly); the archive's own worker object is then never linked.
xiaotai_host_test(port_kv_stress SOURCES kv_stress_test.c LINK xiaotai_port INCLUDE ../common)
xiaotai_host_test(port_mem_host SOURCES mem_host_test.c LINK xiaotai_port)
xiaotai_host_test(port_mem_failure SOURCES mem_failure_test.c LINK Threads::Threads INCLUDE_ONLY xiaotai_port INCLUDE mem_stubs)
xiaotai_host_test(port_mem_unsafe SOURCES mem_failure_test.c INCLUDE_ONLY xiaotai_port INCLUDE mem_stubs)
target_compile_definitions(port_mem_unsafe PRIVATE CONFIG_ESP_TIMER_IN_IRAM=0)
xiaotai_host_test(port_mem_trace_hash SOURCES mem_trace_unavailable_test.c INCLUDE_ONLY xiaotai_port INCLUDE mem_stubs)
target_compile_definitions(port_mem_trace_hash PRIVATE CONFIG_HEAP_TRACE_HASH_MAP=1)
xiaotai_host_test(port_mem_watermark SOURCES mem_watermark_test.c INCLUDE_ONLY xiaotai_port INCLUDE mem_stubs)
target_compile_definitions(port_mem_watermark PRIVATE __riscv=1 CONFIG_SOC_RTC_FAST_MEM_SUPPORTED=1)
xiaotai_host_test(port_mem_nonrtc SOURCES mem_watermark_test.c INCLUDE_ONLY xiaotai_port INCLUDE mem_stubs)
target_compile_definitions(port_mem_nonrtc PRIVATE __riscv=1 __riscv_atomic=1)
