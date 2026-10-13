"""Exercise camera power ownership; pin/format checks use the actual source."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
board = (root / "components/starter_board/starter_board.c").read_text(encoding="utf-8")
media = root / "components/starter_media"
camera = (media / "src/starter_camera.c").read_text(encoding="utf-8")
assert '"src/starter_camera.c"' in (media / "CMakeLists.txt").read_text(encoding="utf-8")
assert "esp32-camera" in (media / "idf_component.yml").read_text(encoding="utf-8")
assert "{0x03,0x82}" in board
for field, expected in {"pin_xclk": -1, "pin_sccb_sda": -1, "pin_sccb_scl": -1,
                        "sccb_i2c_port": 0, "pin_d0": 39, "pin_d1": 40,
                        "pin_d2": 41, "pin_d3": 42, "pin_d4": 15, "pin_d5": 16,
                        "pin_d6": 48, "pin_d7": 47, "pin_vsync": 46,
                        "pin_href": 38, "pin_pclk": 45}.items():
    assert re.search(r"\." + field + r"\s*=\s*" + str(expected) + r"\b", camera), field
assert "GC0308_PID" in camera and "GC2145_PID" not in camera
assert "JPEG_PIXEL_FORMAT_YCbYCr" in camera
functions = []
for name in ("update_reg", "starter_board_camera_power"):
    match = re.search(r"(?:static )?esp_err_t " + name + r"\([^)]*\)\s*\{.*?\n\}", board, re.S)
    assert match, name
    functions.append(match[0])
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef int esp_err_t;
typedef int i2c_master_dev_handle_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(ms) (ms)
static int s_axp=1, s_aw=2, s_control=3;
static bool s_power_ready, locked;
static uint8_t regs[3][256];
static unsigned writes, fail_at, delays;
static void xSemaphoreTake(int sem, int ticks) {assert(sem==3 && !locked); (void)ticks; locked=true;}
static void xSemaphoreGive(int sem) {assert(sem==3 && locked); locked=false;}
static void vTaskDelay(unsigned ticks) {assert(locked && ticks==10); ++delays;}
static esp_err_t write_reg(int dev, uint8_t reg, uint8_t value) {
    assert(locked && (dev==1 || dev==2));
    if (++writes==fail_at) return 99;
    regs[dev][reg]=value; return 0;
}
static esp_err_t i2c_master_transmit_receive(int dev, const uint8_t *reg, unsigned n,
    uint8_t *out, unsigned m, int timeout) {
    assert(locked && n==1 && m==1 && timeout==100);
    *out=regs[dev][*reg]; return 0;
}
'''
main = r'''
int main(void) {
    assert(starter_board_camera_power(true)==ESP_ERR_INVALID_STATE && !writes);
    s_power_ready=true;
    for (unsigned trial=0; trial<1000; ++trial) {
        regs[1][0x90]=0xab; regs[2][0x03]=0x82;
        assert(starter_board_camera_power(true)==0 && !locked);
        assert(regs[1][0x94]==28 && regs[1][0x90]==0xbf && regs[2][0x03]==0x83);
        assert(starter_board_camera_power(false)==0 && !locked);
        assert(regs[1][0x90]==0xab && regs[2][0x03]==0x82);
    }
    for (unsigned step=1; step<=4; ++step) {
        regs[1][0x90]=0xab; regs[2][0x03]=0x82; writes=0; fail_at=step;
        assert(starter_board_camera_power(true)==99 && !locked);
        fail_at=0;
        assert(starter_board_camera_power(false)==0 && !locked);
        assert(regs[1][0x90]==0xab && regs[2][0x03]==0x82);
    }
    for (unsigned step=1; step<=2; ++step) {
        writes=0; fail_at=step;
        assert(starter_board_camera_power(false)==99 && !locked);
    }
    assert(delays>=1000);
}
'''
with tempfile.TemporaryDirectory(prefix="cores3-board-") as tmp:
    path = Path(tmp)
    (path / "test.c").write_text(prefix + "\n".join(functions) + main, encoding="utf-8")
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
print("PASS: GC0308 pin/format contract, camera-only rails/reset, 1000 power cycles and I2C errors")
