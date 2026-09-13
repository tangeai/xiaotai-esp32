"""Compile real media/AEC headers for each platform, without board drivers."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
shared = root / "components/starter_media"
p4 = root.parent / "waveshare-esp32p4-xiaotai/components/starter_media"
cmake = (p4 / "CMakeLists.txt").read_text()
assert '"src/starter_aec.c"' in cmake
assert '${shared}/src/starter_aec.c' not in cmake
p4_source = (p4 / "src/starter_media.c").read_text()
assert 'starter_aec_process_capture(' in p4_source
assert 'starter_aec_submit_capture(' not in p4_source

with tempfile.TemporaryDirectory(prefix="media-platform-contract-") as tmp:
    path = Path(tmp)
    (path / "driver").mkdir()
    (path / "driver/i2c_master.h").write_text("typedef void *i2c_master_bus_handle_t;\n")
    (path / "esp_err.h").write_text("typedef int esp_err_t;\n")
    (path / "starter_tirtc.h").write_text(
        "typedef int starter_tirtc_mode_t; typedef int starter_tirtc_frame_t;\n")
    for is_p4 in (0, 1):
        aec = p4 if is_p4 else shared
        code = '#include "starter_media.h"\n#include "starter_aec.h"\n'
        if is_p4:
            code += '''
_Static_assert(STARTER_AEC_CAPTURE_DMA_CHANNELS == 2, "P4 MR DMA contract");
_Static_assert(sizeof(&starter_aec_process_capture) > 0, "P4 synchronous AEC");
int main(void) {
    starter_media_status_t status = {0};
    status.video_sent = 1;
    status.jpeg_max_encode_us = 2;
    status.jpeg_deadline_misses = 3;
    return status.video_sent + status.jpeg_max_encode_us + status.jpeg_deadline_misses != 6;
}
'''
        else:
            code += '''
_Static_assert(STARTER_AEC_CAPTURE_DMA_CHANNELS == 4, "S3 MMR DMA contract");
_Static_assert(sizeof(&starter_aec_submit_capture) > 0, "S3 asynchronous AEC");
int main(void) { starter_media_status_t status = {0}; return status.audio_sent; }
'''
        (path / "test.c").write_text(code)
        subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra",
                        "-Werror", f"-DCONFIG_IDF_TARGET_ESP32P4={is_p4}",
                        "-I" + str(path), "-I" + str(shared / "include"),
                        "-I" + str(aec / "src"), str(path / "test.c"),
                        "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)
print("PASS: S3 asynchronous MMR and P4 synchronous MR header/ownership contracts")
