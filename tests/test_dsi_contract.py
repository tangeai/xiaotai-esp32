"""Check the IDF-version callback adaptation without changing dependency files."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
PORT = ROOT / 'managed_components/espressif__esp_lvgl_port'
original = PORT / 'src/lvgl9/esp_lvgl_port_disp.c'
before = original.read_bytes()
with tempfile.TemporaryDirectory() as directory:
    temp = Path(directory)
    (temp / 'CMakeLists.txt').write_text(f'''
cmake_minimum_required(VERSION 3.16)
project(dsi_callback NONE)
add_library(lvgl_port_lib OBJECT "{original.as_posix()}")
set_target_properties(lvgl_port_lib PROPERTIES LINKER_LANGUAGE C)
function(idf_component_get_property out component prop)
    set(${{out}} "{PORT.as_posix()}" PARENT_SCOPE)
endfunction()
include("{(ROOT / 'cmake/lvgl_idf6_dsi.cmake').as_posix()}")
get_target_property(selected lvgl_port_lib SOURCES)
if(NOT selected STREQUAL "${{CMAKE_BINARY_DIR}}/xiaotai_lvgl_port/esp_lvgl_port_disp.c")
    message(FATAL_ERROR "DSI adapter did not replace registered source")
endif()
''')
    subprocess.run(['cmake', '-S', str(temp), '-B', str(temp / 'out')], check=True)
    generated = (temp / 'out/xiaotai_lvgl_port/esp_lvgl_port_disp.c').read_text()
    condition = '#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 5, 0)'
    anchor = condition + '\n            cbs.on_frame_buf_complete'
    assert original.read_bytes() == before
    assert anchor not in generated
    start = generated.index(condition)
    block = generated[start:generated.index('#endif', start) + len('#endif')]
    for version, callback in [(50400, 'on_refresh_done'), (50500, 'on_frame_buf_complete'), (60002, 'on_refresh_done')]:
        code = '#define ESP_IDF_VERSION_VAL(a,b,c) ((a)*10000+(b)*100+(c))\n' + block
        output = subprocess.check_output(['cc', '-E', '-P', '-x', 'c', f'-DESP_IDF_VERSION={version}', '-'], input=code, text=True)
        assert output.strip() == f'cbs.{callback} = lvgl_port_flush_dpi_vsync_ready_callback;'
    context_start = generated.index('typedef struct {')
    context_end = generated.index('} lvgl_port_display_ctx_t;', context_start) + len('} lvgl_port_display_ctx_t;')
    start = generated.index('static lv_display_t *lvgl_port_add_disp_priv(', generated.index('* Private functions'))
    end = generated.index('\n#if LVGL_PORT_HANDLE_FLUSH_READY', start)
    remove_start = generated.index('esp_err_t lvgl_port_remove_disp(')
    remove_end = generated.index('\nvoid lvgl_port_flush_ready', remove_start)
    harness = ROOT / 'tests/port_rollback'
    unit = temp / 'port_rollback.c'
    unit.write_text('#include "host.h"\n' + generated[context_start:context_end] + '\n' +
                    generated[start:end] + '\n' + generated[remove_start:remove_end] + '\n#include "cases.h"\n')
    for suffix, flags in [('normal', []), ('asan', ['-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie'])]:
        binary = temp / suffix
        subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Werror',
                        '-I', str(harness), str(unit), '-o', str(binary), *flags], check=True)
        subprocess.run([str(binary)], check=True)
print('DSI callback: IDF 6 uses refresh-completion semaphore, not color-copy completion')
