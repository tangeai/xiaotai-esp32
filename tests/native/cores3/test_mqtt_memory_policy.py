"""Check real esp-mqtt sources retain the CoreS3 payload placement policy."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
# Another board can select a different MQTT version in managed_components.
# Supply CoreS3's immutable fixture when needed; keep the adapter SHA gate.
MQTT = Path(os.environ.get('XIAOTAI_CORES3_MQTT_SOURCE', ROOT / 'managed_components/espressif__mqtt')).resolve()
ADAPTER = ROOT / 'main/boards/m5stack/core-s3/build-support/cmake/mqtt_psram_buffers.cmake'
files = ['mqtt_client.c', 'lib/mqtt_msg.c', 'lib/mqtt_outbox.c']
before = {name: (MQTT / name).read_bytes() for name in files}
with tempfile.TemporaryDirectory() as temporary:
    temp = Path(temporary)
    registered = ';'.join((MQTT / name).as_posix() for name in files)
    (temp / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.16)
project(mqtt_memory NONE)
set(CONFIG_SPIRAM ON)
set(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY ON)
add_library(mqtt_target OBJECT "{registered}")
set_target_properties(mqtt_target PROPERTIES LINKER_LANGUAGE C)
function(idf_component_get_property out component prop)
    if(NOT component STREQUAL "espressif__mqtt")
        message(FATAL_ERROR "Unexpected component owner")
    endif()
    if(prop STREQUAL "COMPONENT_DIR")
        set(${{out}} "{MQTT.as_posix()}" PARENT_SCOPE)
    else()
        set(${{out}} mqtt_target PARENT_SCOPE)
    endif()
endfunction()
include("{ADAPTER.as_posix()}")
starter_enable_mqtt_psram_buffers()
get_target_property(result mqtt_target SOURCES)
foreach(path IN LISTS result)
    if(NOT path MATCHES "/starter_mqtt/")
        message(FATAL_ERROR "Original allocation source still registered")
    endif()
endforeach()
''')
    subprocess.run(['cmake', '-S', str(temp), '-B', str(temp / 'build')], check=True)
    allocations = [
        ('(uint8_t *)malloc(buffer_size)', '(uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)'),
        ('(uint8_t *)calloc(buffer_size, sizeof(uint8_t))', '(uint8_t *)heap_caps_calloc(buffer_size, sizeof(uint8_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)'),
        ('heap_caps_malloc(message->len + message->remaining_len, MQTT_OUTBOX_MEMORY)', 'heap_caps_malloc(message->len + message->remaining_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)'),
    ]
    for name, (old, new) in zip(files, allocations):
        original = before[name].decode().replace('\r\n', '\n')
        generated = (temp / 'build/starter_mqtt' / name).read_text()
        assert original.count(old) == 1
        assert generated == '#include "esp_heap_caps.h"\n' + original.replace(old, new)
        assert (MQTT / name).read_bytes() == before[name]
print('CoreS3 MQTT: only three payload allocations changed; task, failure and free paths retained')
