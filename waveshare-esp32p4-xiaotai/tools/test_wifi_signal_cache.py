#!/usr/bin/env python3
"""Check the actual cached UI accessor and event invalidation, with no Wi-Fi RPC."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root.parent / "lckfb-szpi-esp32s3-tirtc/components/wifi_manager/src/wifi_manager.c").read_text()
def function(signature):
    start = source.index(signature)
    return source[start:source.index("\n}", start) + 2]

accessor = function("bool wifi_manager_signal_dbm(")
assert "esp_wifi_sta_get_ap_info" not in accessor
assert "#define WIFI_SIGNAL_POLL_MS 30000U" in source
worker = function("static void signal_task(")
assert "epoch == s_signal_epoch" in worker
assert "ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(WIFI_SIGNAL_POLL_MS))" in worker
body = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define taskENTER_CRITICAL(x) ((void)(x))
#define taskEXIT_CRITICAL(x) ((void)(x))
static int s_signal_lock;
static bool s_connected;
static void *s_signal_task;
static uint32_t s_signal_epoch;
static uint32_t s_signal_revision;
static int s_cached_rssi=1;
static unsigned notifications;
static void xTaskNotifyGive(void *task) {assert(task); ++notifications;}
'''
body += function("static void signal_connection_changed(") + accessor
body += function("uint32_t wifi_manager_signal_revision(")
body += r'''
int main(void) {
    int8_t rssi=0;
    assert(!wifi_manager_signal_dbm(&rssi));
    s_connected=true;
    signal_connection_changed(); /* safe even before the worker exists */
    assert(wifi_manager_signal_revision()==1);
    assert(!wifi_manager_signal_dbm(&rssi));
    s_signal_task=(void*)1;
    s_cached_rssi=-60;
    assert(wifi_manager_signal_dbm(&rssi) && rssi==-60);
    assert(!wifi_manager_signal_dbm(NULL));
    s_connected=false;
    signal_connection_changed();
    assert(s_cached_rssi==1 && notifications==1 && !wifi_manager_signal_dbm(&rssi));
    s_connected=true;
    signal_connection_changed();
    assert(notifications==2 && !wifi_manager_signal_dbm(&rssi));
    assert(wifi_manager_signal_revision()==3);
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="p4-rssi-") as directory:
    path = Path(directory)
    (path / "test.c").write_text(body)
    subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    str(path / "test.c"), "-o", str(path / "test")], check=True)
    subprocess.run([str(path / "test")], check=True)
product = (root.parent / "lckfb-szpi-esp32s3-tirtc/components/starter_product/src/starter_product.c").read_text()
assert "PRODUCT_WIFI_SIGNAL_PERIOD_MS" not in product
assert "signal_revision != s_wifi_signal_revision" in product
assert "signal_style != s_wifi_signal_style" in product
print("PASS: 30s background RSSI, event invalidation, revision/level-driven UI with no RPC")
