#!/usr/bin/env python3
"""Run real setup UI code before runtime exists and during snapshot contention.

Optional --revision reads an old Git commit without changing the worktree.
LVGL and network stubs verify navigation/text, not physical LCD or Wi-Fi.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--revision")
args = parser.parse_args()


def read(path):
    if args.revision:
        return subprocess.check_output(
            ["git", "show", f"{args.revision}:{path}"], cwd=ROOT
        ).decode("utf-8")
    return (ROOT / path).read_text(encoding="utf-8")


def function(source, name):
    match = re.search(r"^(?:static )?(?:void|bool) " + name + r"\([^;{}]*\)\s*\{", source, re.M)
    assert match, name
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end] + "\n"


product = read("components/starter_product/src/starter_product.c")
ui = read("components/starter_product/src/starter_product_s3_ui.inc")
runtime = read("components/starter_runtime/src/starter_runtime.c")
tick = function(product, "product_tick")
# Execute the real timer prefix, including its early return. Merely finding a
# setup call somewhere later in product_tick would miss the original defect.
start = tick.index("starter_runtime_status_t runtime = starter_runtime_status();")
end = tick.index("starter_media_status_t media = starter_media_status();", start)
prefix = tick[start:end]
call_gate = re.search(r"show_call = call_now &&[^;]+;", tick).group(0)
renderer = function(ui, "s3_render_page")
# Page construction is mocked, but its real post-render path must also avoid
# waiting for a business snapshot on Wi-Fi and binding pages.
render_tail = renderer[renderer.index("    }", renderer.index("default: return false;")) + 5:]

code = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

enum { PAGE_HOME_FACE, PAGE_NETWORK, PAGE_BINDING, PAGE_CALL, PAGE_CONTACTS };
typedef int product_page_t;
enum { STARTER_BINDING_REQUIRED, STARTER_BINDING_CHECKING, STARTER_BINDING_READY,
       STARTER_BINDING_FAILED };
typedef int starter_product_binding_state_t;
enum { STARTER_RUNTIME_WAITING, STARTER_RUNTIME_CALL_ACTIVE };
enum { CALL_RING_NONE, CALL_RING_INCOMING };
enum { ESP_OK, LV_OBJ_FLAG_HIDDEN = 1, WIFI_IF_STA };
#define pdTRUE 1
#define pdMS_TO_TICKS(ms) (ms)
#define TAG "setup-test"
#define ESP_LOGI(...) ((void)0)
#define IPSTR "%u.%u.%u.%u"
#define IP2STR(ip) 192U, 168U, 1U, 2U

typedef struct { char text[640]; bool hidden, enabled; int height; } lv_obj_t;
typedef struct { struct { char ssid[33]; } sta; } wifi_config_t;
typedef struct { unsigned ip; } esp_netif_ip_info_t;
typedef int esp_netif_t;
typedef struct { int state; } starter_runtime_status_t;
typedef struct { int contacts; } starter_runtime_product_snapshot_t;
static struct {
    bool setup_active;
    lv_obj_t *network_text, *network_disconnect, *binding_code, *binding_hint, *binding_retry;
    char network_cache[640];
    int64_t network_due;
} s3_ui;
static atomic_int s_binding_ui_state;
static product_page_t s_page;
static char s_previous_verification_code[17];
static unsigned s_call_ring_kind;
static bool connected, ap_ready, provisioning, manually_disconnected, connection_failed;
static bool reconciling, known_unbound, snapshot_available, control_pending;
static int control_error, runtime_state, renders, touches, business_ticks, snapshot_reads;
static int blocking_reads, business_refreshes;
static int64_t now;
static const char *verification_code = "";
static void *s_product_mutex;
static starter_runtime_product_snapshot_t s_product_snapshot = {7};
static lv_obj_t network_label, disconnect_button, code_label, hint_label, retry_button;

static bool wifi_manager_connected(void) { return connected; }
static bool wifi_manager_provisioning(void) { return ap_ready; }
static bool wifi_manager_manually_disconnected(void) { return manually_disconnected; }
static bool wifi_manager_connection_failed(void) { return connection_failed; }
static bool wifi_manager_control_pending(void) { return control_pending; }
static int wifi_manager_control_error(void) { return control_error; }
static const char *wifi_manager_provisioning_ssid(void) { return "XiaoTai-TEST"; }
static const char *wifi_manager_provisioning_url(void) { return "http://192.168.6.1"; }
static const char *wifi_manager_provisioning_status(void) { return "ready"; }
static int esp_wifi_get_config(int iface, wifi_config_t *out) {
    assert(iface == WIFI_IF_STA); strcpy(out->sta.ssid, "saved-network"); return 0;
}
static esp_netif_t *esp_netif_get_handle_from_ifkey(const char *key) {
    static esp_netif_t netif; assert(key); return &netif;
}
static int esp_netif_get_ip_info(esp_netif_t *netif, esp_netif_ip_info_t *ip) {
    assert(netif && ip); return 0;
}
static bool wifi_manager_signal_dbm(int8_t *rssi) { *rssi = -55; return connected; }
static void starter_runtime_copy_device_id(char *out, size_t n) { snprintf(out, n, "test-device"); }
static bool platform_client_ready(void) { return connected; }
static bool platform_client_mqtt_connected(void) { return connected; }
static bool platform_client_reconciling(void) { return reconciling; }
static bool platform_client_known_unbound(void) { return known_unbound; }
static bool platform_client_provisioning(void) { return provisioning; }
static const char *platform_client_verification_code(void) { return verification_code; }
static starter_runtime_status_t starter_runtime_status(void) {
    return (starter_runtime_status_t){runtime_state};
}
static int xSemaphoreTake(void *mutex, unsigned wait) {
    assert(mutex && (wait == 0 || wait == 50));
    snapshot_reads++; blocking_reads += (wait != 0); return snapshot_available;
}
static void xSemaphoreGive(void *mutex) { assert(mutex); }
static void label_set_text_if_changed(lv_obj_t *obj, const char *text) {
    assert(obj); snprintf(obj->text, sizeof(obj->text), "%s", text);
}
static void set_button_text(lv_obj_t *obj, const char *text) { label_set_text_if_changed(obj, text); }
static void set_object_visible(lv_obj_t *obj, bool visible) { assert(obj); obj->hidden = !visible; }
static lv_obj_t *lv_obj_get_parent(lv_obj_t *obj) { assert(obj); return obj; }
static void lv_obj_set_height(lv_obj_t *obj, int h) { assert(obj); obj->height = h; }
static void s3_set_enabled(lv_obj_t *obj, bool enabled) { assert(obj); obj->enabled = enabled; }
static void lv_obj_add_flag(lv_obj_t *obj, int flag) { assert(obj && flag == 1); obj->hidden = true; }
static void lv_obj_clear_flag(lv_obj_t *obj, int flag) { assert(obj && flag == 1); obj->hidden = false; }
static void note_interaction(void) { touches++; }
static void s3_network_text(void);
static bool refresh_after_render(void);
static int64_t monotonic_ms(void) { return now; }
static void s3_refresh_ui(int64_t time, starter_runtime_status_t state,
                          const starter_runtime_product_snapshot_t *product) {
    (void)time; (void)state; assert(product); business_refreshes++;
}
static void render_page(void) {
    renders++;
    s3_ui.network_text = s3_ui.network_disconnect = NULL;
    s3_ui.binding_code = s3_ui.binding_hint = s3_ui.binding_retry = NULL;
    s3_ui.network_cache[0] = '\0'; s3_ui.network_due = 0;
    if (s_page == PAGE_NETWORK) {
        s3_ui.network_text = &network_label; s3_ui.network_disconnect = &disconnect_button;
        s3_network_text();
    } else if (s_page == PAGE_BINDING) {
        s3_ui.binding_code = &code_label; s3_ui.binding_hint = &hint_label;
        s3_ui.binding_retry = &retry_button;
    }
    assert(refresh_after_render());
}
'''
code += function(runtime, "starter_runtime_try_product_snapshot")
code += runtime[runtime.index("starter_runtime_product_snapshot_t starter_runtime_product_snapshot(void)"):
                runtime.index("const char *starter_runtime_state_name")]
code += function(ui, "s3_network_text")
code += function(ui, "s3_update_setup")
code += "static bool refresh_after_render(void) {\n" + render_tail + "\n"
code += "static void frame(void) {\n" + prefix + "\n (void)runtime; business_ticks++;\n}\n"
code += "static bool show_call(bool call_now, bool wifi_connected) { bool show_call;\n"
code += call_gate + "\nreturn show_call; }\n"
code += r'''
int main(void) {
    (void)s3_update_setup; /* Older revisions never reach setup in the prefix. */
    (void)platform_client_provisioning; (void)platform_client_verification_code;
    /* Boot without Wi-Fi/runtime: page appears before AP initialization. */
    runtime_state = STARTER_RUNTIME_WAITING;
    atomic_store(&s_binding_ui_state, STARTER_BINDING_REQUIRED);
    frame();
    assert(s_page == PAGE_NETWORK && s3_ui.setup_active);
    assert(strstr(network_label.text, "Wi-Fi") && business_ticks == 0);
    assert(snapshot_reads == 0 && s_product_mutex == NULL);
    puts("PASS: unconfigured boot displays setup without runtime");

    /* The AP becomes ready later; do not freeze the page's first text. */
    ap_ready = true; now = 5000; frame();
    assert(strstr(network_label.text, "XiaoTai-TEST"));
    assert(strstr(network_label.text, "192.168.6.1"));
    assert(disconnect_button.hidden && renders == 1);
    int rendered = renders;
    for (int i = 0; i < 30; i++) { now += 1000; frame(); }
    assert(renders == rendered && business_ticks == 0 && touches >= 30);
    puts("PASS: delayed AP details refresh; waiting does not rebuild or sleep");

    /* Wi-Fi connected, clock/Report/MQTT not ready: show a placeholder. */
    connected = true; ap_ready = false; now += 100; frame();
    assert(s_page == PAGE_BINDING && !strcmp(code_label.text, "------"));
    assert(retry_button.hidden && business_ticks == 0);
    provisioning = true; verification_code = "123456"; now += 100; frame();
    assert(!strcmp(code_label.text, "123456"));
    assert(strstr(hint_label.text, "等待绑定"));
    rendered = renders;
    for (int i = 0; i < 300; i++) { now += 100; frame(); }
    assert(renders == rendered && business_ticks == 0);
    verification_code = "654321"; frame();
    assert(!strcmp(code_label.text, "654321"));
    puts("PASS: first binding code updates throughout three prompt-length intervals");

    provisioning = false; atomic_store(&s_binding_ui_state, STARTER_BINDING_FAILED); frame();
    assert(!retry_button.hidden && !strcmp(code_label.text, "------"));
    assert(strstr(hint_label.text, "重试"));
    atomic_store(&s_binding_ui_state, STARTER_BINDING_REQUIRED); frame();
    assert(retry_button.hidden && strstr(hint_label.text, "获取验证码"));
    assert(blocking_reads == 0 && business_refreshes == 0);
    puts("PASS: binding failure and manual retry remain visible before runtime");

    /* Stored credentials take the same setup route; success alone exits it. */
    atomic_store(&s_binding_ui_state, STARTER_BINDING_CHECKING); frame();
    assert(s_page == PAGE_BINDING && strstr(hint_label.text, "检查"));
    s_product_mutex = &s_product_snapshot; snapshot_available = true;
    atomic_store(&s_binding_ui_state, STARTER_BINDING_READY); frame();
    assert(s_page == PAGE_HOME_FACE && !s3_ui.setup_active && business_ticks == 1);
    s_page = PAGE_CALL; runtime_state = STARTER_RUNTIME_CALL_ACTIVE;
    frame(); assert(s_page == PAGE_CALL && show_call(true, true));
    puts("PASS: successful binding resumes home and normal call presentation");

    /* Contention retains business state but must not obstruct unbind/offline. */
    snapshot_available = false; int before = business_ticks;
    frame(); assert(s_page == PAGE_CALL && business_ticks == before);
    assert(s_product_snapshot.contacts == 7);
    int reads_before = blocking_reads, refreshes_before = business_refreshes;
    reconciling = known_unbound = true; s_call_ring_kind = CALL_RING_INCOMING; frame();
    assert(s_page == PAGE_BINDING && s3_ui.setup_active && !show_call(true, true));
    assert(s_call_ring_kind == CALL_RING_NONE && business_ticks == before);
    assert(blocking_reads == reads_before && business_refreshes == refreshes_before);
    connected = false; manually_disconnected = true; frame();
    assert(s_page == PAGE_NETWORK && strstr(network_label.text, "开启热点"));
    assert(blocking_reads == reads_before && business_refreshes == refreshes_before);
    ap_ready = true; now += 1000; frame();
    assert(strstr(network_label.text, "XiaoTai-TEST") && !show_call(true, false));
    puts("PASS: contention, unbind and Wi-Fi loss preserve setup priority");

    control_error = 1; now += 1000; frame();
    assert(strstr(network_label.text, "热点启动失败"));
    control_error = 0; ap_ready = false; manually_disconnected = false;
    connection_failed = true; now += 1000; frame();
    assert(strstr(network_label.text, "重连已保存"));
    puts("PASS: portal errors and saved-network retry update independently");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="p4-setup-") as directory:
    path = Path(directory)
    source = path / "test.c"
    source.write_text(code, encoding="utf-8")
    binary = path / "test"
    subprocess.run([os.environ.get("CC", "gcc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-g", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Navigation must run once, before any snapshot-dependent early exit, and the
# call branch must not overwrite an unbind/setup page during teardown.
assert tick.count("s3_update_setup(") == 1
assert prefix.index("s3_update_setup(") < prefix.index("starter_runtime_try_product_snapshot(")
assert "!s3_ui.setup_active" in call_gate
assert "s3_ui.network_due" not in function(ui, "s3_refresh_ui")
print("PASS: timer ordering and a single setup/network refresh owner")
