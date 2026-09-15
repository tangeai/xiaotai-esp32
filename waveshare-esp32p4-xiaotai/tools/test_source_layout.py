#!/usr/bin/env python3
"""Keep one product implementation and the required P4 board/media inputs."""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
for obsolete in (
    "main/app_main.c", "main/app_version.h", "main/ui", "main/application",
    "main/connectivity", "main/protocols", "main/debug", "main/drivers/audio",
    "main/drivers/virtual_audio", "main/drivers/virtual_video",
    "tools/generate_home_assets.py",
):
    path = root / obsolete
    # Git does not track empty directories left by a local cleanup.
    assert not path.is_file(), obsolete
    if path.is_dir():
        assert not any(item.is_file() for item in path.rglob("*")), obsolete

main_cmake = (root / "main/CMakeLists.txt").read_text(encoding="utf-8")
assert 'SRCS "xiaotai_main.c"' in main_cmake
hardware_cmake = (root / "components/p4_hardware/CMakeLists.txt").read_text(encoding="utf-8")
for relative in re.findall(r'"\$\{ref\}/([^\"]+)"', hardware_cmake):
    assert (root / "main" / relative).exists(), relative

for relative in (
    "drivers/camera/camera_driver.c", "drivers/display/display_driver.c",
    "media/camera_pipeline.c", "services/call_video_renderer.c",
    "platform/app_memory_policy.c",
):
    assert '${ref}/' + relative in hardware_cmake, relative

config = (root / "main/Kconfig.projbuild").read_text(encoding="utf-8")
for obsolete in (
    "AUDIO_LOCAL_LOOPBACK_TEST", "APP_SERIAL_CALL_CLI_ENABLE",
    "APP_DEBUG_SCREEN_SERVER_ENABLE", "APP_DEBUG_SCREEN_SERVER_PORT",
    "APP_DEVICE_ONLINE_PAUSE_DURING_RTC", "APP_MEMORY_WATERLINE_LOG",
    "APP_TIRTC_QUERY_SEND_BUFFER_USED", "APP_RTC_WAIT_VIDEO_SUBSCRIBE_BEFORE_CAPTURE",
):
    assert not re.search(r'^config ' + obsolete + r'$', config, re.MULTILINE), obsolete

ui = (root / "components/starter_product/src/starter_product_s3_ui.inc").read_text(encoding="utf-8")
dispatch = ui[ui.index("static bool s3_render_page("):]
dispatch = dispatch[:dispatch.index("\n}\n")]
for page in (
    "HOME_FACE", "HOME_CLOCK", "MENU", "AI_CHAT", "CONTACTS", "CONTACT_DETAIL",
    "ROOM", "ROOM_FORM", "SETTINGS", "NETWORK", "BINDING", "CALL", "CALL_RESULT",
    "EMOJIS", "EMOJI_PREVIEW", "DIAGNOSTICS",
):
    assert "case PAGE_" + page + ":" in dispatch, page
assert '#if CONFIG_IDF_TARGET_ESP32P4\n        render_call(screen);' in dispatch
print("PASS: one product UI, all page routes and retained P4 camera/video inputs")
