"""Compile the camera-absent implementation and verify CoreS3-SE build inputs."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
media = root / "components/starter_media"
cmake = (media / "CMakeLists.txt").read_text(encoding="utf-8")
assert '"src/starter_camera_unavailable.c"' in cmake
assert '"src/starter_camera.c"' not in cmake
assert "esp32-camera" not in (media / "idf_component.yml").read_text(encoding="utf-8")
header = (media / "include/starter_media.h").read_text(encoding="utf-8")
status = re.search(r"typedef enum \{\s*STARTER_CAMERA_OFF,.*?\} starter_camera_status_t;", header, re.S)
assert status
with tempfile.TemporaryDirectory(prefix="cores3-board-") as tmp:
    path = Path(tmp)
    (path / "esp_err.h").write_text("typedef int esp_err_t;\n#define ESP_ERR_NOT_SUPPORTED 0x106\n")
    (path / "starter_media.h").write_text("#include <stdint.h>\n" + status[0] +
        "\nstarter_camera_status_t starter_media_camera_status(void);\n")
    (path / "test.c").write_text(r'''
#include <assert.h>
#include <string.h>
#include "starter_camera.h"
#include "starter_media.h"
int main(void) {
    const uint32_t generations[] = {0, 1, UINT32_MAX};
    starter_camera_status_t empty = {0};
    for (unsigned i = 0; i < sizeof(generations)/sizeof(generations[0]); ++i) {
        assert(starter_camera_prepare(generations[i]) == ESP_ERR_NOT_SUPPORTED);
        starter_camera_set_session(generations[i]);
        starter_camera_status_t actual = starter_media_camera_status();
        assert(memcmp(&actual, &empty, sizeof(actual)) == 0);
        assert(actual.phase == STARTER_CAMERA_OFF && actual.error == 0);
    }
}
''')
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-I", str(path), "-I", str(media / "src"),
                    str(media / "src/starter_camera_unavailable.c"), str(path / "test.c"),
                    "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: CoreS3-SE camera absence, generation-independent NOT_SUPPORTED and empty status")
