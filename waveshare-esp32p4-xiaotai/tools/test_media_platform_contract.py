"""Compile this project\'s real media/AEC headers without board drivers."""
import os
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
media = root / "components/starter_media"
cmake = (media / "CMakeLists.txt").read_text()
source = (media / "src/starter_media.c").read_text()
assert '"src/starter_aec.c"' in cmake
assert 'starter_aec_process_capture(' in source
assert 'starter_aec_submit_capture(' not in source

with tempfile.TemporaryDirectory(prefix="media-platform-contract-") as tmp:
    path = Path(tmp)
    (path / "driver").mkdir()
    (path / "driver/i2c_master.h").write_text("typedef void *i2c_master_bus_handle_t;\n")
    (path / "esp_err.h").write_text("typedef int esp_err_t;\n")
    (path / "starter_tirtc.h").write_text(
        "typedef int starter_tirtc_mode_t; typedef int starter_tirtc_frame_t;\n")
    for is_p4 in (1,):
        aec = media
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
                        "-I" + str(path), "-I" + str(media / "include"),
                        "-I" + str(aec / "src"), str(path / "test.c"),
                        "-o", str(path / "test")], check=True)
        subprocess.run([str(path / "test")], check=True)
print("PASS: P4 local synchronous MR header/ownership contract")
