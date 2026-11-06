"""Build-local adapter plus original/patched SPI worker behavior with fake IO."""
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tests/regression'))
from test_voice_reliability import function

hosted = ROOT / 'managed_components/espressif__esp_hosted'
original = hosted / 'host/mcu/eh_host_mcu_transport/src/eh_host_bus_spi.c'
before = original.read_bytes()
adapter = ROOT / 'main/boards/wt99p461-s1/build-support/cmake/hosted_spi_diagnostics.cmake'
with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    (temp / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.20)
project(hosted_spi C)
set(CONFIG_ESP_HOSTED_HOST_TRANSPORT_MCU_READY ON)
set(CONFIG_ESP_HOSTED_HOST_TRANSPORT_BUS_SPI_READY ON)
add_subdirectory("{(hosted / 'host/mcu/eh_host_mcu_transport').as_posix()}" transport)
function(idf_component_get_property out component prop)
    set(${{out}} "{hosted.as_posix()}" PARENT_SCOPE)
endfunction()
include("{adapter.as_posix()}")
xiaotai_enable_hosted_spi_diagnostics()
get_target_property(selected eh_host_mcu_transport SOURCES)
list(FIND selected "${{CMAKE_BINARY_DIR}}/xiaotai_hosted/eh_host_bus_spi.c" generated_index)
list(FIND selected "src/eh_host_bus_spi.c" original_index)
if(generated_index EQUAL -1 OR NOT original_index EQUAL -1)
    message(FATAL_ERROR "SPI adapter did not replace the registered source")
endif()
''')
    subprocess.run(['cmake', '-S', str(temp), '-B', str(temp / 'out')], check=True)
    generated = temp / 'out/xiaotai_hosted/eh_host_bus_spi.c'
    assert original.read_bytes() == before
    patched = generated.read_text()
    diagnostics = patched[patched.index('/* Temporary counters:'):patched.index('#endif', patched.index('/* Temporary counters:'))]
    for name, source in [('original', original), ('diagnostics', generated)]:
        unit = temp / f'{name}.c'
        unit.write_text('#include "host.h"\n' + (diagnostics if name == 'diagnostics' else '') +
                        function(str(source), 'eh_spi_rx_task') + '\n#include "cases.h"\n')
        for mode, flags in [('normal', []), ('asan', ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie'])]:
            binary = temp / f'{name}-{mode}'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g',
                            *(['-DDIAGNOSTICS'] if name == 'diagnostics' else []),
                            '-I', str(ROOT / 'tests/hosted_spi'), str(unit), '-o', str(binary), *flags], check=True)
            subprocess.run([str(binary)], check=True)
print('WT SPI diagnostics: reviewed source identity, original behavior and counters passed')
