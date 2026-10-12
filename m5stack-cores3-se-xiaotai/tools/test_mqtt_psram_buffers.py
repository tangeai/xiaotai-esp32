"""Host checks for the MQTT allocation-only adapter; no device access."""
import hashlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


CMAKE = shutil.which("cmake")
ROOT = Path(__file__).resolve().parents[1]
ADAPTER = ROOT / "cmake" / "mqtt_psram_buffers.cmake"


@unittest.skipUnless(CMAKE, "CMake is needed for allocation adapter checks")
class MqttPsramBuffersTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mqtt memory ")
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)
        self.source = self.directory / "upstream.c"
        self.output = self.directory / "generated" / "mqtt.c"

    def adapt(self, original, before, after, expected_hash=None):
        raw = original.encode("utf-8")
        self.source.write_bytes(raw)
        digest = expected_hash or hashlib.sha256(
            raw.replace(b"\r\n", b"\n")).hexdigest()
        script = self.directory / "check.cmake"
        script.write_text(
            f'include("{ADAPTER.as_posix()}")\n'
            f'starter_mqtt_psram_source("{self.source.as_posix()}" '
            f'"{self.output.as_posix()}" "{digest}" '
            f'[[{before}]] [[{after}]])\n', encoding="utf-8")
        result = subprocess.run([CMAKE, "-P", str(script)],
                                capture_output=True, text=True)
        self.assertEqual(raw, self.source.read_bytes(), "upstream must not change")
        return result

    def test_only_data_allocation_changes_and_crlf_is_accepted(self):
        allocations = (
            ("(uint8_t *)malloc(buffer_size)",
             "(uint8_t *)heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)"),
            ("(uint8_t *)calloc(buffer_size, sizeof(uint8_t))",
             "(uint8_t *)heap_caps_calloc(buffer_size, sizeof(uint8_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)"),
            ("heap_caps_malloc(message->len + message->remaining_len, MQTT_OUTBOX_MEMORY)",
             "heap_caps_malloc(message->len + message->remaining_len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)"),
        )
        for before, after in allocations:
            with self.subTest(allocation=before):
                original = (
                    f"/* upstream license */\r\nbuffer = {before};\r\n"
                    "if (!buffer) return ESP_ERR_NO_MEM;\r\n"
                    "free(buffer);\r\nvTaskDelete(NULL);\r\n")
                result = self.adapt(original, before, after)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(self.output.read_text(encoding="utf-8"),
                                 '#include "esp_heap_caps.h"\n' +
                                 original.replace("\r\n", "\n").replace(before, after))

    def test_changed_upstream_fails_without_output(self):
        result = self.adapt("malloc(size)", "malloc(size)", "replacement(size)", "0" * 64)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("MQTT source changed", result.stderr)
        self.assertFalse(self.output.exists())

    def test_missing_allocation_fails_without_output(self):
        result = self.adapt("changed_allocation(size)", "malloc(size)", "replacement(size)")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("MQTT allocation anchor missing", result.stderr)
        self.assertFalse(self.output.exists())

    def test_idf_wrapper_directory_resolves_submodule_sources(self):
        # Exercise real target replacement, not only the text patch helper.
        wrapper = self.directory / "mqtt"
        files = ("mqtt_client.c", "lib/mqtt_msg.c", "lib/mqtt_outbox.c")
        for relative in files:
            path = wrapper / "esp-mqtt" / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("/* fixture */", encoding="utf-8")
        registered = ";".join((wrapper / "esp-mqtt" / f).as_posix() for f in files)
        (self.directory / "CMakeLists.txt").write_text(f'''
cmake_minimum_required(VERSION 3.16)
project(mqtt_layout NONE)
set(CONFIG_SPIRAM ON)
set(CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY ON)
add_library(mqtt_target OBJECT "{registered}")
set_target_properties(mqtt_target PROPERTIES LINKER_LANGUAGE C)
function(idf_component_get_property out component prop)
    if(prop STREQUAL "COMPONENT_DIR")
        set(${{out}} "{wrapper.as_posix()}" PARENT_SCOPE)
    else()
        set(${{out}} mqtt_target PARENT_SCOPE)
    endif()
endfunction()
include("{ADAPTER.as_posix()}")
# Allocation/hash behavior is covered separately with the real helper above.
function(starter_mqtt_psram_source source output expected_hash before after)
    configure_file("${{source}}" "${{output}}" COPYONLY)
endfunction()
starter_enable_mqtt_psram_buffers()
get_target_property(result mqtt_target SOURCES)
foreach(path IN LISTS result)
    if(NOT path MATCHES "/starter_mqtt/")
        message(FATAL_ERROR "Original source was not replaced: ${{path}}")
    endif()
endforeach()
''', encoding="utf-8")
        result = subprocess.run([CMAKE, "-S", str(self.directory), "-B",
                                 str(self.directory / "build")], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
