#!/usr/bin/env python3
"""Prevent an old prewarmed encoder from retaining the singleton workspace."""
from pathlib import Path

root = Path(__file__).resolve().parents[1]
source = (root / "components/p4_hardware/p4_video.c").read_text()
poll = source[source.index("void p4_video_poll(void)"):source.index("void p4_video_key_frame")]
config = poll.index("media_governor_set_rtc_video_config(&config)")
start = poll.index("camera_pipeline_set_rtc_video_enabled(true)")
assert "camera_pipeline_on_rtc_video_config_changed()" in poll[config:start], (
    "profile switch must reconcile the idle reserved encoder before starting capture; "
    "otherwise the old encoder retains the singleton output workspace (reserved=1/1)"
)
assert poll.index("camera_pipeline_is_running()) return") < config
print("PASS: stopped owner -> new profile -> reserved encoder reconciliation -> capture start")
