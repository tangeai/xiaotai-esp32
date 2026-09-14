#!/usr/bin/env python3
"""Portable source/ELF build gates. Requires only IDF Python and its binutils.

These checks preserve the review invariants from check_*.sh. They do not prove
runtime correctness. Host C/ASan tests belong to run_host_tests.sh, not linking.
"""
import argparse
import os
from pathlib import Path
import re
import subprocess
import sys


FILES = {
    "product": "components/starter_product/src/starter_product.c",
    "s3_ui": "components/starter_product/src/starter_product_s3_ui.inc",
    "s3_face": "components/starter_product/src/starter_product_s3_face.inc",
    "platform": "components/platform_client/src/platform_client.c",
    "media": "components/starter_media/src/starter_media.c",
    "aec": "components/starter_media/src/starter_aec.c",
    "aec_header": "components/starter_media/src/starter_aec.h",
    "voice": "components/starter_voice/src/starter_voice.c",
    "backend": "components/starter_voice/src/wake_backend.cpp",
    "runtime": "components/starter_runtime/src/starter_runtime.c",
    "tirtc": "components/starter_tirtc/src/starter_tirtc.c",
    "wifi": "components/wifi_manager/src/wifi_manager.c",
    "dns": "components/wifi_manager/src/captive_dns.c",
    "main": "main/app_main.c",
    "defaults": "sdkconfig.defaults",
    "resolved": "sdkconfig",
    "lock": "dependencies.lock",
    "media_cmake": "components/starter_media/CMakeLists.txt",
    "media_manifest": "components/starter_media/idf_component.yml",
    "button": "components/starter_button/src/starter_button.c",
    "console": "components/starter_console/src/starter_console.c",
}


class PolicyFailure(Exception):
    pass


def require(condition, reason):
    if not condition:
        raise PolicyFailure(reason)


def has_line(text, pattern):
    """Like grep/rg without multiline: never match across source lines."""
    return any(re.search(pattern, line) for line in text.splitlines())


def line_range(text, start, end):
    """Select inclusive review regions as in the original sed range checks."""
    selected = []
    active = False
    for line in text.splitlines():
        if not active:
            if re.search(start, line):
                active = True
                selected.append(line)
        else:
            selected.append(line)
            if re.search(end, line):
                active = False
    return "\n".join(selected)


class Context:
    def __init__(self, args):
        self.root = args.root.resolve()
        self.elf = args.elf.resolve() if args.elf else None
        self.nm = args.nm
        self.objdump = args.objdump
        self.product = args.product
        self.text_cache = {}
        self.symbol_cache = {}

    def path(self, name):
        if name == "product" and self.product:
            return self.product.resolve()
        return self.root / FILES.get(name, name)

    def text(self, name):
        path = self.path(name)
        if path not in self.text_cache:
            # UTF-8 source, independent of the Windows active code page. Path
            # handles native Windows/Unix syntax; no WSL or shell translation.
            self.text_cache[path] = path.read_text(encoding="utf-8-sig")
        return self.text_cache[path]

    def compact(self, name):
        return re.sub(r"[ \t\r\n\v\f]+", "", self.text(name))

    def need(self, name, *tokens, compact=False):
        text = self.compact(name) if compact else self.text(name)
        for token in tokens:
            require(token in text, f"{self.path(name)}: missing invariant: {token}")

    def forbid(self, name, pattern):
        require(not has_line(self.text(name), pattern),
                f"{self.path(name)}: forbidden pattern: {pattern}")

    def tool(self, executable, *args):
        require(self.elf is not None and self.elf.is_file(),
                f"linked ELF is missing: {self.elf}")
        # No shell: spaces and non-ASCII paths remain single arguments, and the
        # exact IDF cross tool is used rather than an unrelated PATH binary.
        result = subprocess.run([executable, *args, str(self.elf)],
                                stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                text=True, encoding="utf-8", errors="replace")
        require(result.returncode == 0,
                f"{executable} exited {result.returncode}: {result.stderr.strip()}")
        return result.stdout

    def symbols(self, option):
        if option not in self.symbol_cache:
            self.symbol_cache[option] = self.tool(self.nm, option)
        return self.symbol_cache[option]


def i2c_driver_family(c):
    symbols = c.symbols("-C")
    legacy = has_line(symbols, r"\si2c_driver_install$")
    modern = has_line(symbols, r"\si2c_new_master_bus$")
    require(not (legacy and modern), "ELF links both legacy I2C and driver_ng")
    # A legacy global constructor can survive even after install() is removed
    # by linker GC. Check the map as well as public ELF symbols.
    map_path = c.elf.with_suffix(".map")
    if map_path.is_file():
        link_map = map_path.read_text(encoding="utf-8", errors="replace").replace("\\", "/")
        require(not has_line(link_map, r"driver/libdriver\.a\(i2c\.c\.obj\)|esp_lcd_panel_io_i2c_v1\.c\.obj"),
                "link map contains a legacy I2C object that aborts at boot")


def product_touch_i2c(c):
    c.need("product", "starter_media_i2c_bus()")
    require(has_line(c.text("product"), r"touch_io_config\.scl_speed_hz\s*=\s*TOUCH_I2C_HZ;"),
            "driver_ng touch panel IO must configure its I2C speed")
    c.forbid("product", r"\(uint32_t\)\s*TOUCH_I2C_PORT|driver/i2c\.h")


def backlight_polarity(c):
    c.need("product", "#define LCD_BACKLIGHT_ON_LEVEL 0",
           "#define LCD_BACKLIGHT_OFF_LEVEL 1",
           "awake ? LCD_BACKLIGHT_ON_LEVEL : LCD_BACKLIGHT_OFF_LEVEL")
    c.forbid("product", "ledc_")


def binding_prompt(c):
    c.need("platform", "/v1/device/tts?code=%s", "report->temp_token", "MQTT_EVENT_SUBSCRIBED")
    c.need("product", "platform_client_verification_code()", "PAGE_BINDING", "BINDING_CODE_TOP_LEVEL")
    c.need("media", "starter_media_play_pcm8k")
    c.need("main", "VERIFICATION_PROMPT_REPEAT_COUNT 3U")
    c.forbid("platform", "verification code: %s")


def wifi_provisioning(c):
    c.need("wifi", '"XiaoTai-%02X%02X"', "ap.ap.authmode = WIFI_AUTH_OPEN",
           '#define WIFI_SETUP_URL "http://192.168.6.1"', "ESP_NETIF_DOMAIN_NAME_SERVER",
           "captive_dns_start(s_ap_netif)", "httpd_register_err_handler",
           "bool wifi_manager_connection_failed(void)", "s_has_saved_credentials",
           "else if (!s_provisioning)")
    c.forbid("wifi", r"WIFI_SETUP_PASSWORD|ap\.ap\.authmode = WIFI_AUTH_WPA|ESP_NETIF_CAPTIVEPORTAL_URI")
    c.need("dns", "wildcard DNS ready on UDP/53")
    c.need("wifi", "stop_provisioning()", "config.open_fn = portal_open",
           "portal_socket_allowed(httpd_req_to_sockfd(request))")
    got_ip = c.text("wifi").split("event_id == IP_EVENT_STA_GOT_IP", 1)[-1]
    require("atomic_store(&s_control_error, stop_provisioning());" in got_ip,
            "STA recovery must close the provisioning lifecycle and report errors")
    c.need("dns", "esp_err_t captive_dns_stop(void)", "SO_RCVTIMEO", "SO_SNDTIMEO",
           ".sin_addr.s_addr = ip_info.ip.addr", "atomic_store(&s_dns_running, false)")
    if not c.product:
        c.need("s3_ui", "wifi_manager_disconnect()", "if (!wifi_manager_connected()) return true;",
               "target = PAGE_NETWORK;", "s3_ui.setup_active = true;",
               "target = PAGE_BINDING;", '"断开当前连接", 8, 188, 304, 48')
    c.need("product", "无需密码，系统通常会提示打开配网页",
           'PRODUCT_WIFI_CONNECTED_TEXT "Wi-Fi 连接成功"',
           'PRODUCT_WIFI_FAILED_TEXT "Wi-Fi 连接失败，请重新配网"',
           "s_pending_wifi_notice", "s_wifi_notice != WIFI_NOTICE_NONE", "!wifi_notice_visible")


def s3_product_ui(c):
    c.need("product", '#include "starter_product_s3_ui.inc"',
           '#include "starter_product_s3_face.inc"', "s3_render_page(screen)")
    home = line_range(c.text("s3_ui"), r"^static void s3_render_home\(", r"^}")
    for token in ('render_header(screen, "小钛")', "s3_ui.caption_panel = s3_scroller(",
                  "lv_label_set_long_mode(s_subtitle, LV_LABEL_LONG_WRAP)",
                  "2 * lv_font_get_line_height(",
                  "lv_obj_set_style_height(s3_ui.caption_panel, caption_height, 0)",
                  "lv_obj_align(s_subtitle, LV_ALIGN_BOTTOM_MID, 0, 0)",
                  "lv_obj_align(s3_ui.caption_panel, LV_ALIGN_BOTTOM_MID, 0, -bottom_inset)",
                  "const lv_coord_t bottom_inset = 2;",
                  "LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE |",
                  "LV_OBJ_FLAG_OVERFLOW_VISIBLE", "LV_SCROLLBAR_MODE_OFF",
                  "create_expression_face(screen, (320 - FACE_CANVAS_W) / 2,",
                  "(240 - FACE_CANVAS_H) / 2)",
                  'make_button(screen, "", 276, 150, 44, 44,',
                  "S3_ACTION_CLOCK", "S3_ACTION_WECHAT",
                  "lv_obj_set_pos(s_wifi_signal, 238, 7)",
                  's3_icon(screen, LV_SYMBOL_BULLET " " LV_SYMBOL_BULLET " " LV_SYMBOL_BULLET,',
                  "280, 6, 32, ACTION_MENU)", "lv_obj_set_height(more, 24)",
                  "lv_obj_set_ext_click_area(more, 20)",
                  "lv_obj_set_style_bg_opa(more, LV_OPA_20, 0)",
                  "lv_obj_set_style_bg_opa(more, LV_OPA_30, LV_STATE_PRESSED)",
                  "LV_OPA_TRANSP", "lv_obj_move_foreground(more)"):
        require(token in home, f"actual S3 home missing invariant: {token}")
    require(all(token not in home for token in ('"唤醒"', '"休眠"',
                                                "S3_ACTION_AI_TOGGLE", "S3_ACTION_AI_STOP")),
            "home must not create a wake or sleep button")
    c.forbid("s3_ui", r"s3_ui\.ai_action|lv_obj_t\s*\*ai_action")
    refresh = line_range(c.text("s3_ui"), r"^static void s3_refresh_ui\(", r"^}")
    for token in ("starter_runtime_caption_read(&s3_ui.caption)",
                  "lv_label_set_text_static(s_subtitle, s3_ui.caption.text)"):
        require(token in refresh, f"actual S3 home refresh missing invariant: {token}")
    require("LV_STATE_SCROLLED" not in refresh and "lv_obj_scroll_to_y(s3_ui.caption_panel" not in refresh,
            "home captions must always follow the latest lines without history scroll gating")
    wechat = line_range(c.text("s3_ui"), r"^static void s3_draw_wechat\(", r"^}")
    c.need("s3_ui", '#include "starter_product_s3_phone.inc"')
    require("&s3_phone_shortcut_image" in wechat and "lv_draw_img(" in wechat,
            "S3 WeChat shortcut must draw the supplied phone image")
    routes = line_range(c.text("s3_ui"), r"^static bool s3_render_page\(", r"^}")
    for target in ("home", "settings", "contacts", "contact_detail", "call", "network", "binding", "emojis"):
        require(f"s3_render_{target}(screen)" in routes, f"S3 page route missing: {target}")
    actions = line_range(c.text("s3_ui"), r"^static bool s3_handle_action\(", r"^}")
    for token in ("s3_ai_request_pending()", "starter_runtime_ai_stop()",
                  "starter_runtime_call_contact(selected)", "starter_runtime_contacts_refresh()"):
        require(token in actions, f"actual S3 action handler missing: {token}")
    c.need("s3_face", "#if LV_COLOR_16_SWAP", "if (front > back) *pixel = colour;",
           "if (overlay)", "lv_color_mix(s3_face.palette[16], *pixel,")
    c.need("s3_face", "FACE_GLASSES", "FACE_MUSIC", "FACE_ZZZ", "FACE_WINK",
           'else if (strcmp(key, "speech") == 0) key = "listening";')


def product_ui(c):
    # The shared file still contains legacy/P4 layout. It cannot stand in for
    # the include that actually renders S3. --product explicitly checks legacy.
    if not c.product:
        s3_product_ui(c)
    tap = line_range(c.text("product"), "static void on_home_tap", r"^}")
    require("PAGE_AI_CHAT" not in tap, "home tap must keep the conversation on the expressive home page")
    require("s_page = PAGE_HOME_FACE;" in tap, "home conversation must render on the expressive home face")
    c.need("product", 'render_header(screen, "小钛")', "PAGE_EMOJI_PREVIEW", "ACTION_EMOJI_APPLY",
           "static const char *const s_emoji_keys[]",
           '"neutral", "happy", "laughing", "funny", "sad", "angry", "crying"',
           '"loving", "embarrassed", "surprised", "shocked", "thinking"',
           '"winking", "cool", "relaxed", "delicious", "kissy", "confident"',
           '"sleepy", "silly", "confused"')
    for name in ("neutral", "happy", "sad", "angry", "surprised", "thinking",
                 "listening", "ambient", "speech", "sleepy"):
        c.need("product", f'strcmp(effective, "{name}")')
    c.need("product", '"正在聆听"', '"AI 正在思考"', '"AI 回复中"',
           "HOME_SUBTITLE_TRANSPARENT", "HOME_MENU_FLOATING_DOTS", "HOME_BRAND_ROBOT_MARK",
           "HOME_WIFI_REAL_RSSI", "HOME_WIFI_SIGNAL_BARS", "HOME_WIFI_ICON_ONLY_RIGHT",
           "lv_obj_set_pos(s_wifi_signal, 292, 5)", "HEADER_BACK_NATIVE_CHEVRON",
           "lv_label_set_text(chevron, LV_SYMBOL_LEFT)", "wifi_manager_signal_dbm(&rssi)",
           '"说“你好小钛”或点击 AI 聊天"', '"小钛在这儿"', '"发会儿呆…"', '"...zzZ"',
           "PRODUCT_IDLE_DAYDREAM_MS 45000", "PRODUCT_IDLE_SLEEPY_MS 180000",
           "PRODUCT_UI_TASK_PRIORITY 6", "PRODUCT_UI_TASK_CORE 0",
           ".task_priority = PRODUCT_UI_TASK_PRIORITY", ".task_affinity = PRODUCT_UI_TASK_CORE",
           "PRODUCT_UI_REFRESH_SLOW_US", "label_set_text_if_changed", "clear_expression_part",
           "refresh_settings_controls", "static void enter_settings_page(void)",
           "static bool page_ends_ai(product_page_t page)", "(void)enter_page(PAGE_SETTINGS);",
           "PAGE_DIAGNOSTICS", "s_diagnostics_due_ms = now + 1000;", "enter_settings_page();",
           "s_settings_volume", "CALL_PERSISTENT_CONTROLS", "refresh_call_controls(runtime, &product)",
           "s_call_accept_button", "set_object_visible(s_call_hangup_button, !incoming)",
           "call ringtone %s", "runtime.state == STARTER_RUNTIME_CALL_CONNECTING")
    c.forbid("product", "s_wifi_signal_value|你好小钛，和我聊天")


def time_sync(c):
    c.need("platform", "ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(",
           'ESP_SNTP_SERVER_LIST("ntp.aliyun.com", "pool.ntp.org")', "esp_netif_sntp_sync_wait")
    c.need("defaults", "CONFIG_LWIP_SNTP_MAX_SERVERS=2")
    c.need("main", "NTP_SYNC_BACKGROUND_TASK", 'setenv("TZ", "CST-8", 1)',
           "xTaskCreateStaticPinnedToCore(starter_start_task", "nvs_worker_init()")
    startup = line_range(c.text("main"), r"^static void starter_start_task\(void \*argument\)", r"^}")
    clock = startup.find("err = platform_client_sync_clock();")
    binding = startup.find("if (!credentials_valid)")
    runtime = startup.find("starter_runtime_start(")
    tirtc = startup.find("starter_tirtc_start(")
    require(0 <= clock < binding < runtime < tirtc,
            "both identity paths must synchronize the clock before cloud startup")
    c.need("platform", "s_clock_synchronized && time(NULL) > 1700000000",
           "err == ESP_OK && time(NULL) > 1700000000")


def tirtc_startup_order(c):
    # Rebind helpers also start the platform; only bootstrap execution order matters here.
    startup = line_range(c.text("main"), r"^static void starter_start_task\(void \*argument\)", r"^}")
    lines = startup.splitlines()
    tirtc = next((i for i, s in enumerate(lines) if "starter_tirtc_start(&tirtc)" in s), None)
    platform = next((i for i, s in enumerate(lines) if "platform_client_start(&platform)" in s), None)
    require(tirtc is not None and platform is not None, "cannot locate TiRTC/platform bootstrap calls")
    require(tirtc < platform, "TiRTC must reserve contiguous internal heap before platform MQTT/TLS")
    body = line_range(c.text("main"), r"^void app_main\(void\)", r"^}")
    require(not has_line(body, "starter_console_start|starter_button_start"),
            "development controls must not reserve stacks before TiRTC startup")
    c.need("main", "TIRTC_FIRST_CONTIGUOUS_HEAP")
    require(re.search(r"(?m)^#if CONFIG_XIAOTAI_DEVELOPMENT_CONSOLE\s+"
                      r"console_err = starter_console_start\(\);\s+#endif", startup),
            "development console must remain behind its opt-in config")
    c.need("tirtc", "TIRTC_TGTRP_POLL_TIMEOUT_MS 20", "TiRtcSetOption(TIRTC_OPT_TGTRP_POLL_TIMEOUT")


def realtime_connect_memory(c):
    lines = c.text("runtime").splitlines()
    require(sum(bool(re.search(r"starter_tirtc_(ai|voip|call)_connect\(", s)) for s in lines) == 3,
            "unexpected number of TiRTC external-connect call sites")
    require(sum(bool(re.search(r"if \(!prepare_external_connect_memory\(\)\)", s)) for s in lines) == 3,
            "every TiRTC external connect must check memory and release its reserve")
    c.need("runtime", "VOIP_CONNECT_TASK_STACK_BYTES (24U * 1024U)",
           "xTaskCreateWithCaps(voip_connect_task", "restore_external_connect_memory();",
           "EXTERNAL_CONNECT_MQTT_FREE_BYTES (64U * 1024U)",
           "EXTERNAL_CONNECT_MQTT_LARGEST_BYTES (32U * 1024U)",
           "s_external_connect_reserve_loaned", "platform_client_mqtt_connected()",
           "EXTERNAL_CONNECT_RESERVE_BYTES (17U * 1024U)",
           "starter_runtime_arm_external_connect_reserve();", "heap_caps_free(s_external_connect_reserve);")
    c.need("platform", "esp_mqtt_client_stop(mqtt)", "esp_mqtt_client_destroy(mqtt)")
    c.need("main", "starter_runtime_arm_external_connect_reserve() != ESP_OK")


def tirtc_thread_stack(c):
    c.need("components/tirtc_sdk/CMakeLists.txt", "src/tirtc_task_stack_policy.c",
           "--wrap=freertos_ThreadCreateWithStackSize")
    c.need("components/tirtc_sdk/src/tirtc_task_stack_policy.c",
           "TIRTC_RTC_THREAD_MIN_STACK_BYTES (24 * 1024)", "__wrap_freertos_ThreadCreateWithStackSize")
    if c.elf is not None:
        require(c.elf.is_file() and c.elf.stat().st_size > 0, "linked ELF is missing or empty")
        require("__wrap_freertos_ThreadCreateWithStackSize" in c.symbols("-g"),
                "linked ELF does not contain the TiRTC task-create wrapper")


def voice_intent(c):
    voice_dir = c.root / "components/starter_voice/src"
    require(voice_dir.is_dir(), f"voice source directory is missing: {voice_dir}")
    for file in [c.path("main"), *sorted(p for p in voice_dir.rglob("*") if p.is_file())]:
        c.forbid(str(file), r"esp_mn_|s_multinet|esp_srmodel_init|recognizer_start")
    require(not (c.root / "components/starter_voice/assets/multinet7/srmodels.bin").exists(),
            "MultiNet must not return with dedicated wake integration")
    c.need("voice", "wake_window_snapshot", "wake_result_check", "s_cooldown_ms = now + 2500",
           "wake_audio_queue_push(s_input", "b->epoch != input_epoch",
           "input_epoch != atomic_load(&s_input_epoch)")
    c.need("components/starter_voice/src/wake_window.h", "epoch != current_epoch")
    body = line_range(c.text("voice"), r"^esp_err_t starter_voice_feed_pcm16k\(", r"^}")
    require(not has_line(body, "xSemaphore|vTaskDelay|heap_caps_"), "wake producer must be bounded and nonblocking")
    c.need("backend", "MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT", "tflite::VerifyModelBuffer", "kws_postprocess")
    c.need("media", "starter_voice_feed_pcm16k(clean.wake_pcm_16k", "captured_ms - clean.wake_delay_ms")
    c.need("aec", ".wake_pcm_16k = uplink", ".wake_delay_ms = STARTER_AGC_DELAY_MS",
           ".wake_pcm_16k = s_aec.clean", ".wake_delay_ms = 0")
    c.need("components/starter_media/src/starter_agc.h", "#define STARTER_AGC_DELAY_MS 10U")
    c.need("button", "starter_runtime_ai_start")
    c.forbid("backend", "i2s_|TiRtc|starter_runtime_|starter_product_")


def wake_image(c):
    symbols = c.symbols("--defined-only")
    require(not has_line(symbols, r" (esp_mn_[A-Za-z0-9_]*|esp_srmodel_init|[A-Za-z0-9_]*multinet[A-Za-z0-9_]*)$"),
            "removed MultiNet/model-loader implementation remains linked")
    require(has_line(symbols, r" wake_backend_run$"), "wake_backend_run missing from ELF")
    require(has_line(symbols, r" _binary_nihaoxiaotai_tflite_start$"), "embedded Voicute model missing from ELF")


def session_priority(c):
    c.need("runtime", "call preempts foreground owner=%s", "产品优先级：来电高于 AI/H5",
           "starter_tirtc_accept_h5(false);", "AI session closed by remote/server idle timeout",
           "AI session ended by remote end_session/idle policy", "#define RUNTIME_TASK_STACK_BYTES 24576U")
    c.need("product", "wake ignored while foreground state=%s", "CALL_RING_AI_ACK", "在呢。",
           "ai_ack_female_8k_wav_start", "ai_ack_male_8k_wav_start", "ack_voice", "回应声  %s")
    c.need("voice", "wake_result_check", "s_cooldown_ms = now + 2500")


def audio_only(c):
    require(not has_line(c.symbols("--defined-only"),
            r" (esp_camera_[A-Za-z0-9_]*|cam_task|ll_cam_[A-Za-z0-9_]*|frame2jpg(_cb)?|fmt2jpg(_cb)?|camera_task|starter_jpeg_collect|starter_tirtc_send_(mjpeg|h264)|starter_tirtc_video_ready|on_request_key_frame)$"),
            "S3 image still contains camera/video producers")
    for name in ("media_cmake", "media_manifest", "lock"):
        c.forbid(name, "esp32-camera")
    # test_audio_only.py compiles/runs host C with ASan, not Xtensa firmware.
    # It remains mandatory in the separate, existing run_host_tests.sh suite.


def media_cpu_fairness(c):
    body = line_range(c.text("media"), "static void audio_capture_task", "static size_t decode_audio_item")
    require("vTaskDelay(pdMS_TO_TICKS(MEDIA_CPU_YIELD_MS))" in body,
            "backlogged AEC capture must block cooperatively instead of starving IDLE/rtc_thread")
    c.need("voice", "vTaskDelay(pdMS_TO_TICKS(20))")
    for name in ("media", "voice", "defaults"):
        c.forbid(name, r"esp_task_wdt_(delete|deinit)|CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU[01]=n")
    c.need("media", "#defineMEDIA_REALTIME_CORE1",
           'xTaskCreatePinnedToCoreWithCaps(audio_capture_task,"board_audio_tx",6144,NULL,7,NULL,0,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)',
           "update_max_counter(&s_aec_max_process_us,aec_elapsed_us)", compact=True)


def i2s_resource(c):
    count = sum(bool(re.search(r"^\s*esp_err_t\s+[^=]+=\s*i2s_new_channel\(", s))
                for s in c.text("media").splitlines())
    require(count == 1, f"full-duplex audio must allocate paired TX/RX once; found {count} calls")
    c.need("media", "#defineI2S_AUDIO_PORTI2S_NUM_0", "i2s_new_channel(&audio_channel,&s_i2s_tx,&s_i2s_rx)",
           "i2s_channel_init_std_mode(s_i2s_tx,&tx_config)", "i2s_channel_init_tdm_mode(s_i2s_rx,&rx_config)",
           "I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,I2S_SLOT_MODE_STEREO)",
           "I2S_TDM_SLOT0|I2S_TDM_SLOT1|I2S_TDM_SLOT2|I2S_TDM_SLOT3",
           "audio_codec_new_i2s_data(&i2s_cfg)", "esp_codec_dev_open(s_speaker_dev,&speaker_format)",
           "esp_codec_dev_open(s_microphone_dev,&microphone_format)", ".channel=AUDIO_TDM_SLOTS",
           "ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0)|ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1)", compact=True)
    require(c.text("media").count("I2S_MCLK_MULTIPLE_256") >= 2, "TX and four-slot RX must both use 256 Fs MCLK")
    c.need("media", "EXT_RAM_BSS_ATTRstaticint16_ts_play_stereo", "s_play_stereo[output_index]=current",
           "s_play_stereo[output_index+2U]=midpoint", "AUDIO_PLAYBACK_I2S_VALUES_PER_INPUT*sizeof(int16_t)",
           "esp_codec_dev_write(s_speaker_dev,s_play_stereo,bytes)",
           "esp_codec_dev_read(s_microphone_dev,capture,capture_bytes)",
           "esp_codec_dev_set_out_vol(s_speaker_dev,70)",
           "esp_codec_dev_set_in_channel_gain(s_microphone_dev,", compact=True)
    compact = c.compact("media")
    for token in ("i2s_channel_enable(s_i2s_tx)", "i2s_channel_enable(s_i2s_rx)",
                  "es8311_sample_frequency_config(", "es7210_init("):
        require(token not in compact, f"codec adapter must own I2S enable/configuration: {token}")
    c.forbid("media", r"I2S_NUM_1|i2s_channel_reconfig_(std|tdm)_gpio")
    body = line_range(c.text("media"), "static void audio_capture_task", "static size_t decode_audio_item")
    require("s_playback_active" not in body, "full-duplex capture must continue during downlink playback")


def audio_capture(c):
    c.need("aec_header", "STARTER_AEC_MIC_SLOT=0", compact=True)
    require(re.search(r"starter_tirtc_send_alaw\(.*>=0\)", c.compact("media")) is not None,
            "TiRTC media send success must accept non-negative return values")
    c.need("main", "#defineDISCOVERY_URLCONFIG_XIAOTAI_DISCOVERY_URL", ".max_send_buffer_bytes=256U*1024U", compact=True)
    c.need("platform", "#definePLATFORM_DEFAULT_DISCOVERYCONFIG_XIAOTAI_DISCOVERY_URL", compact=True)
    require("TiRtcSetOption(TIRTC_OPT_SERVICE_ENDPOINT" not in c.compact("tirtc"),
            "platform discovery transport must not override the TiRTC SDK endpoint")
    for config in ("CONFIG_MBEDTLS_TLS_SERVER_AND_CLIENT=y", "CONFIG_MBEDTLS_SSL_PROTO_TLS1_2=y"):
        require(config in c.text("defaults").splitlines(), f"deferred TLS requires {config}")


def aec(c):
    require(has_line(c.text("media_manifest"), r'version:\s*"==2\.4\.7"'),
            "Espressif esp-sr must be pinned to 2.4.7 in manifest")
    sr_lock = line_range(c.text("lock"), "espressif/esp-sr:", "^  [^ ]")
    require(has_line(sr_lock, r"version:\s*2\.4\.7"), "esp-sr lock version must be 2.4.7")
    c.need("aec_header", "STARTER_AEC_SAMPLE_RATE_HZ=16000", "STARTER_AEC_TRANSPORT_RATE_HZ=8000",
           "STARTER_AEC_CAPTURE_DMA_CHANNELS=4", "STARTER_AEC_MIC_CHANNELS=2",
           "STARTER_AEC_SECOND_MIC_SLOT=2", "STARTER_AEC_MIC_SLOT=0", "STARTER_AEC_REFERENCE_SLOT=1", compact=True)
    c.need("aec", 'afe_config_init("MMR",NULL,AFE_TYPE_FD,AFE_MODE_LOW_COST)',
           "config->aec_mode=AEC_MODE_FD_LOW_COST", "config->aec_nlp_level=AEC_NLP_LEVEL_AGGR",
           "config->afe_linear_gain=STARTER_AEC_OUTPUT_GAIN", "config->afe_perferred_core=0",
           "config->se_init=true", "config->vad_enable_channel_trigger=true", "config->fixed_output_channel=false",
           "config->memory_alloc_mode=AFE_MEMORY_ALLOC_INTERNAL_PSRAM_BALANCE",
           "s_aec.iface->get_feed_channel_num(s_aec.handle)!=3", "s_aec.iface->get_fetch_channel_num(s_aec.handle)!=1",
           "s_aec.iface->feed(s_aec.handle,input->pcm)", "s_aec.iface->fetch_with_delay(s_aec.handle,pdMS_TO_TICKS(200))",
           "vQueueDeleteWithCaps(s_aec.pending_slots)",
           "s_aec.tdm[i*STARTER_AEC_CAPTURE_DMA_CHANNELS+STARTER_AEC_SECOND_MIC_SLOT]",
           "s_aec.tdm[i*STARTER_AEC_CAPTURE_DMA_CHANNELS+STARTER_AEC_MIC_SLOT]",
           "s_aec.tdm[i*STARTER_AEC_CAPTURE_DMA_CHANNELS+STARTER_AEC_REFERENCE_SLOT]",
           "starter_audio_resampler_16k_to_8k_process(&s_aec.resampler,", compact=True)
    c.need("components/starter_media/src/starter_audio_resampler.h", "STARTER_AUDIO_RESAMPLER_TAPS 31U")
    c.need("components/starter_media/src/starter_audio_resampler.c", "resampler->emit_phase")
    c.forbid("aec", r"sum\s*/\s*2|clean\[i \* 2U\]")
    # The ADC owner now reuses its measured read-completion time. Preserve
    # timestamp units, epoch ownership and successful-read ordering.
    capture = line_range(c.text("media"), r"^static void audio_input_task\(", r"^}")
    capture = re.sub(r"\s+", "", capture)
    steps = (
        "uint32_tcapture_epoch=atomic_load(&s_capture_epoch);",
        "uint32_tmute_epoch=atomic_load(&s_mute_epoch);",
        "esp_codec_dev_read(s_microphone_dev,capture,capture_bytes)==ESP_CODEC_DEV_OK",
        "int64_tread_end=esp_timer_get_time();",
        "if(read_ok){",
        "starter_aec_submit_capture(capture_bytes,read_end/1000,capture_epoch,mute_epoch)",
    )
    positions = [capture.find(step) for step in steps]
    require(all(position >= 0 for position in positions) and positions == sorted(positions),
            "AEC input must submit a successful ADC read with its completion timestamp in ms and original epochs")
    c.need("media", ".channel_mask=ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0)|ESP_CODEC_DEV_MAKE_CHANNEL_MASK(1)|ESP_CODEC_DEV_MAKE_CHANNEL_MASK(2)|ESP_CODEC_DEV_MAKE_CHANNEL_MASK(3)",
           "#defineAUDIO_TRANSPORT_SAMPLE_RATE_HZ8000U", "#defineAUDIO_HW_SAMPLE_RATE_HZ16000U",
           "#defineAUDIO_MCLK_MULTIPLE256U",
           "starter_aec_fetch(&clean)", "clean.mute_epoch!=mute_epoch||clean.capture_epoch!=capture_epoch",
           "packet_pcm[packet_samples++]=clean.pcm_8k[i]",
           "AUDIO_PLAYBACK_I2S_VALUES_PER_INPUT(AUDIO_PLAYBACK_UPSAMPLE*2U)",
           "s_play_stereo[output_index+2U]=midpoint", compact=True)
    c.need("media_cmake", '"src/starter_aec.c"', "esp-sr", compact=True)
    for name in ("media", "aec_header"):
        c.forbid(name, r"s_playback_active|STARTER_AEC_REFERENCE_SLOT\s*=\s*0")
    for name in ("aec", "media_cmake"):
        c.forbid(name, r"starter_mic_select|aec_process\(")


def downlink_audio(c):
    c.need("tirtc", "downlink accepted mode=", "downlink rejected mode=", "不能把本端的发送流号当成")
    c.forbid("tirtc", "frame->stream_id == CALL_AUDIO_STREAM")
    c.need("media", "s_audio_decoded", "s_audio_played", "s_audio_playback_blocked", "s_audio_write_failed",
           "s_pcm8k_cancel_sequence", "PCM8k prompt interrupted for realtime media")
    c.need("console", "Downlink: decoded=")


def lvgl_pool_placement(c):
    if c.elf is None:
        return
    sizes = re.findall(r"^CONFIG_LV_MEM_SIZE_KILOBYTES=(\d+)$", c.text("resolved"), re.M)
    require(len(sizes) == 1, "LVGL fixed pool size is missing from sdkconfig")
    pools = re.findall(r"^([0-9a-fA-F]+)\s+([0-9a-fA-F]+)\s+[bBdD]\s+"
                       r"work_mem_int(?:[.$][^\s]*)?$", c.symbols("-S"), re.M)
    require(len(pools) == 1, "cannot identify the linked LVGL fixed pool")
    address, size = (int(value, 16) for value in pools[0])
    require(0x3C000000 <= address and address + size <= 0x3E000000,
            "LVGL fixed pool must be linked into S3 PSRAM")
    require(size == int(sizes[0]) * 1024, "linked LVGL pool differs from sdkconfig size")


def memory_placement(c):
    for name in ("defaults", "resolved"):
        for config in ("CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y",
                       "# CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP is not set",
                       "CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM=y", "CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM=4",
                       "CONFIG_ESP_WIFI_RX_BA_WIN=4", "CONFIG_ESP_WIFI_DYNAMIC_TX_BUFFER=y"):
            require(config in c.text(name).splitlines(), f"{c.path(name)} must contain {config}")
    c.need("main", "heap_caps_register_failed_alloc_callback(allocation_failed)", "platform_client_run_request_loop()",
           "#defineSTARTER_TASK_STACK_BYTES13312U",
           'xTaskCreateStaticPinnedToCore(starter_start_task,"platform_main",STARTER_TASK_STACK_BYTES,NULL,4,s_start_stack,&s_start_tcb,tskNO_AFFINITY)',
           "staticEXT_RAM_BSS_ATTRStackType_ts_start_stack[STARTER_TASK_STACK_BYTES/sizeof(StackType_t)]",
           "staticDRAM_ATTRStaticTask_ts_start_tcb", "platform_request_task(NULL)", compact=True)
    c.need("runtime", 'xTaskCreateWithCaps(runtime_task,"starter_session",RUNTIME_TASK_STACK_BYTES,NULL,6,&task,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)', compact=True)
    c.need("main", "platform task PSRAM stack remaining=%u bytes")
    c.forbid("main", "starter task internal stack|PLATFORM_REQUEST_TASK_STACK_BYTES")
    c.need("product", ".task_stack_caps=MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT", compact=True)
    c.need("CMakeLists.txt", "starter_enable_lvgl_psram_pool()")
    c.need("cmake/lvgl_psram_pool.cmake", "TARGET_DIRECTORY", "COMPILE_DEFINITIONS",
           "LV_ATTRIBUTE_LARGE_RAM_ARRAY=EXT_RAM_BSS_ATTR")
    for declaration in (
            "starter_runtime_product_snapshot_t s_product_snapshot",
            "char s_call_connect_peer", "char s_call_connect_token",
            "char s_call_wx_session_token", "char s_call_wx_payload"):
        c.need("runtime", "static EXT_RAM_BSS_ATTR " + declaration)
    c.need("runtime",
           "event->text=heap_caps_malloc(length+1U,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)",
           "voip_connect_request_t*request=heap_caps_calloc(1,sizeof(*request),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)",
           "ai_credentials_t*credentials=heap_caps_calloc(1,sizeof(*credentials),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)",
           compact=True)
    c.need("product", "static EXT_RAM_BSS_ATTR char s_ai_history",
           "static EXT_RAM_BSS_ATTR starter_product_contact_t")
    save = line_range(c.text("product"), r"^static void preferences_save\(void\)", r"^}")
    require("nvs_worker_submit_latest(preferences_write_job," in save and "&s_preferences" in save and
            "xTaskCreate" not in save, "settings must reuse the latest-snapshot writer")
    writer = line_range(c.text("product"), r"^static esp_err_t preferences_write\(", r"^}")
    require("s_preferences." not in writer and "err = nvs_commit(nvs)" in writer,
            "NVS must write an owned snapshot and check commit failures")
    c.need("product", "preferences save not queued: %s", "nvs_worker_call(preferences_load_job")
    c.need("components/nvs_worker/src/nvs_worker.c",
           "static EXT_RAM_BSS_ATTR nvs_job_t s_jobs[NVS_WORKER_SLOTS]",
           "static DRAM_ATTR StackType_t s_stack", "#define NVS_WORKER_STACK_BYTES 4096U",
           'xTaskCreateStaticPinnedToCore(worker_task, "nvs_worker"',
           "job->abandoned = true", "job->state == SLOT_QUEUED")
    c.need("components/nvs_worker/include/nvs_worker.h", "#define NVS_WORKER_SLOTS 4U",
           "#define NVS_WORKER_DATA_BYTES 512U")
    c.need("wifi", "config.task_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT", "config.stack_size = 8192")
    for adapter in ("load_credentials_job", "save_credentials_job", "forget_credentials_job"):
        c.need("wifi", "nvs_worker_call(" + adapter)
    for adapter in ("load_tirtc_job", "save_tirtc_job", "clear_tirtc_job"):
        c.need("components/runtime_config/src/runtime_config.c", "nvs_worker_call(" + adapter)
    boot = line_range(c.text("main"), r"^void app_main\(void\)", r"^}")
    require(boot.index("init_nvs();") < boot.index("nvs_worker_init()") < boot.index("starter_product_start()"),
            "NVS worker must be ready before any adapters")
    startup = line_range(c.text("runtime"), r"^esp_err_t starter_runtime_start\(", r"^}")
    require("xTaskCreateWithCaps(" in startup and "starter_tirtc_set_handlers(" in startup,
            "cannot locate runtime task/callback startup")
    require(startup.index("xTaskCreateWithCaps(") < startup.index("starter_tirtc_set_handlers("),
            "runtime callbacks must not publish partially initialized resources")
    require("vSemaphoreDelete(mutex)" in startup and "vQueueDelete(queue)" in startup and
            startup.index("vSemaphoreDelete(mutex)") < startup.index("s_product_mutex = mutex"),
            "runtime failure must release only unpublished resources")
    require(startup.index("platform_client_set_online_handler(") < startup.index("xTaskNotifyGive(task)"),
            "runtime worker must wait for complete initialization")
    c.need("runtime", "ulTaskNotifyTake(pdTRUE, portMAX_DELAY)")
    lvgl_pool_placement(c)
    c.need("button", 'xTaskCreateWithCaps(button_task,"ai_button",BUTTON_TASK_STACK_BYTES,NULL,BUTTON_TASK_PRIORITY,&s_button_task,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)', compact=True)
    require("xTaskCreate(request_task" not in c.compact("platform"), "platform must not create another request task")
    c.need("platform", "heap_caps_calloc(PLATFORM_REQUEST_QUEUE_DEPTH,sizeof(*s_request_pool),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)",
           "heap_caps_malloc(PLATFORM_HTTP_BODY_MAX,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)",
           "xQueueCreate(PLATFORM_REQUEST_QUEUE_DEPTH,sizeof(uint8_t))", compact=True)
    c.need("media", "heap_caps_calloc(AUDIO_RX_QUEUE_DEPTH,sizeof(*s_audio_rx_pool),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)",
           "s_audio_rx_ready_queue=xQueueCreateWithCaps(AUDIO_RX_QUEUE_DEPTH,sizeof(uint8_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)",
           "s_audio_rx_free_queue=xQueueCreateWithCaps(AUDIO_RX_QUEUE_DEPTH,sizeof(uint8_t),MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)",
           'xTaskCreateWithCaps(audio_sink_task,"board_audio_rx",6144,NULL,8,NULL,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)',
           'xTaskCreatePinnedToCoreWithCaps(audio_capture_task,"board_audio_tx",6144,NULL,7,NULL,0,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)',
           'xTaskCreatePinnedToCoreWithCaps(audio_input_task,"board_audio_in",4096,NULL,9,NULL,MEDIA_REALTIME_CORE,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)', compact=True)
    c.need("aec", "config->memory_alloc_mode=AFE_MEMORY_ALLOC_INTERNAL_PSRAM_BALANCE", "heap_caps_aligned_calloc(",
           "#defineAFE_CAPS(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT)",
           'xTaskCreatePinnedToCoreWithCaps(afe_feed_worker,"afe_feed",8192,NULL,7,&s_aec.feed_worker,1,AFE_CAPS)', compact=True)


def entry_frame_bytes(disassembly, symbol):
    found = False
    raw = ""
    for line in disassembly.splitlines():
        if f"<{symbol}>:" in line:
            found = True
            continue
        if found:
            match = re.match(r"\s*[0-9a-fA-F]+:\s+([0-9a-fA-F]+)\b", line)
            if match:
                raw = match[1].lower()
                break
    require(len(raw) >= 6, f"cannot read Xtensa entry instruction for {symbol}")
    entry = raw[-6:]
    require(entry[-2:] == "36" and entry[3] == "1",
            f"{symbol} does not start with the expected Xtensa entry a1 instruction")
    return (int(entry[2], 16) + int(entry[:2], 16) * 16) * 8


def ai_stack(c):
    c.need("tirtc", "heap_caps_get_free_size(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA)",
           "heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA)",
           'log_start_memory("pre-init");', 'log_start_memory("pre-start");rc=TiRtcStart(', compact=True)
    runtime = c.text("runtime")
    sizes = re.findall(r"^#define RUNTIME_TASK_STACK_BYTES ([0-9]+)U$", runtime, re.M)
    if not sizes:
        sizes = re.findall(r'xTaskCreate\(runtime_task, "starter_session", ([0-9]+),', runtime)
    require(len(sizes) == 1, "cannot unambiguously resolve starter_session stack size")
    task = int(sizes[0])
    frames = {name: entry_frame_bytes(c.tool(c.objdump, "-d", f"--disassemble={name}"), name)
              for name in ("runtime_task", "handle_ai_token", "starter_tirtc_ai_connect", "TiRtcWhipConnect")}
    known = sum(frames.values())
    reserve = task - known
    require(frames["handle_ai_token"] <= 512, "handle_ai_token frame exceeds 512 bytes; move buffers off the stack")
    require(task <= 24576, f"starter_session stack is {task} bytes; reviewed PSRAM budget is 24576")
    require(reserve >= 4096, f"starter_session has only {reserve} bytes for nested SDK/RTOS calls; require 4096")
    print(f"AI/TLS stack: task={task} known-chain={known} nested-reserve={reserve} token-frame={frames['handle_ai_token']}")


# Preserve the original POST_BUILD ordering and one named result per policy.
POLICIES = {
    "i2c_driver_family": i2c_driver_family,
    "product_touch_i2c": product_touch_i2c,
    "backlight_polarity": backlight_polarity,
    "binding_prompt": binding_prompt,
    "wifi_provisioning": wifi_provisioning,
    "product_ui": product_ui,
    "time_sync": time_sync,
    "tirtc_startup_order": tirtc_startup_order,
    "realtime_connect_memory": realtime_connect_memory,
    "tirtc_thread_stack": tirtc_thread_stack,
    "voice_intent": voice_intent,
    "wake_image": wake_image,
    "session_priority": session_priority,
    "audio_only": audio_only,
    "media_cpu_fairness": media_cpu_fairness,
    "i2s_resource": i2s_resource,
    "audio_capture": audio_capture,
    "aec": aec,
    "downlink_audio": downlink_audio,
    "memory_placement": memory_placement,
    "ai_stack": ai_stack,
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--elf", type=Path)
    parser.add_argument("--nm", default=os.environ.get("CROSS_NM", "xtensa-esp32s3-elf-nm"))
    parser.add_argument("--objdump", default=os.environ.get("CROSS_OBJDUMP", "xtensa-esp32s3-elf-objdump"))
    parser.add_argument("--product", type=Path, help="single-source legacy UI check override")
    parser.add_argument("--policy", choices=["all", *POLICIES], default="all")
    args = parser.parse_args()
    if args.policy == "all" and args.elf is None:
        parser.error("--elf is required for the complete post-build gate")
    context = Context(args)
    names = list(POLICIES) if args.policy == "all" else [args.policy]
    for name in names:
        try:
            POLICIES[name](context)
        except (PolicyFailure, OSError, UnicodeError) as error:
            print(f"FAIL: {name}: {error}", file=sys.stderr)
            return 1
        print(f"PASS: {name}")
    if args.policy == "all":
        print("Build policies passed; host C/ASan tests and device regression are separate and were not run.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
