#!/usr/bin/env python3
"""Summarize ESP32-P4 media performance from a serial log.

The analyzer deliberately uses the firmware's existing low-rate statistics.
It does not require extra runtime tracing or change the media data path.
"""

from __future__ import annotations

import argparse
import re
import statistics
import sys
from dataclasses import dataclass, field
from pathlib import Path


TIMESTAMP_RE = re.compile(r"^(\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2})")
UPTIME_RE = re.compile(r"(?:^|\s)[IWE] \((\d+)\)")
TARGET_RE = re.compile(r"target=(\d+)x(\d+)@(\d+)")
ACTIVE_RUNTIME_RE = re.compile(r"camera=1 rtc=1 (\d+)x(\d+) fps=([0-9.]+) bitrate=(\d+)kbps")
CLOSE_SNAPSHOT_RE = re.compile(
    r"rtc close snapshot:.*?age_ms=(\d+).*?"
    r"tx\[attempt=(\d+) fail=(\d+) v=(\d+)/(\d+)KB a=(\d+)/(\d+)KB\].*?"
    r"sdk_buf=(\d+)/(\d+)"
)
DOWNLINK_RE = re.compile(
    r"rx=([0-9.]+)fps/(\d+)kbps queued=([0-9.]+)fps "
    r"decoded=([0-9.]+)fps converted=([0-9.]+)fps presented=([0-9.]+)fps.*?"
    r"drop=input:(\d+) display:(\d+) fail=decode:(\d+) convert:(\d+).*?"
    r"sync=create:(\d+) restart:(\d+) reset:(\d+) overflow:(\d+)"
)
PAYLOAD_RE = re.compile(r"payload\[min/avg/max\]=(\d+)/(\d+)/(\d+)")
LUMA_SOURCE_CHANGE_RE = re.compile(r"luma_src_change=(\d+)/(\d+)")
LUMA_ENCODER_CHANGE_RE = re.compile(r"luma_enc_change=(\d+)/(\d+)")
COMPACT_CAMERA_RE = re.compile(
    r"CAM (\d+)x(\d+)@(\d+) f=([0-9.]+) br=(\d+)k "
    r"gap=(\d+)/(\d+)ms us=c/s/e/cb/l:(\d+)/(\d+)/(\d+)/(\d+)/(\d+) "
    r"(?:motion=s/e:[0-9.]+/[0-9.]+ chg=\d+/\d+,\d+/\d+ )?"
    r"drop=(\d+)/(\d+)/(\d+)/(\d+) drain=(\d+) fail=(\d+)/(\d+)/(\d+)"
)
COMPACT_CAMERA_MOTION_RE = re.compile(
    r"motion=s/e:([0-9.]+)/([0-9.]+) chg=(\d+)/(\d+),(\d+)/(\d+)"
)
COMPACT_TX_RE = re.compile(
    r"VTX f=([0-9.]+) br=(\d+)k ok=(\d+) "
    r"e/b/d/t/s=(\d+)/(\d+)/(\d+)/(\d+)/(\d+).*?"
    r"q=(\d+)/(\d+) sb=(\d+)/(\d+) rx=(\d+)/(\d+)/(\d+)/(\d+)ms"
)
COMPACT_DOWNLINK_RE = re.compile(
    r"VRX (\d+)x(\d+) in=([0-9.]+)/(\d+)k "
    r"q/d/c/o=([0-9.]+)/([0-9.]+)/([0-9.]+)/([0-9.]+) "
    r"drop=(\d+)/(\d+) fail=(\d+)/(\d+) age=(\d+)/(\d+)ms "
    r"depth=(\d+)/(\d+) buf=(\d+)/(\d+)@(\d+)ms "
    r"ms=au/cvt/ppa:(\d+)/(\d+)/(\d+) "
    r"(?:kd=(\d+)/(\d+) )?gap=(\d+)/(\d+) "
    r"old=(\d+)/(\d+) reset=(\d+)/(\d+)(?: ovf=(\d+))?"
)
COMPACT_DOWNLINK_PARTS_RE = re.compile(
    r"parts=pack/swap/ui:(\d+)/(\d+)/(\d+) "
    r"max=au/cvt/ppa:(\d+)/(\d+)/(\d+)"
)
TGTRP_DIAG_RE = re.compile(
    r"TGTRP-DIAG.*?rtx/lost=(\d+)/(\d+).*?rx_stall=(\d+)ms "
    r"rtt/rto=(\d+)/(\d+).*?sendq=(\d+)/(\d+).*?"
    r"pktbuf=(\d+)/(\d+).*?delivered=(\d+).*?drop=(\d+)"
)
RTC_POLL_STALL_RE = re.compile(r"rtc_thread_poll_socks\s+(\d+)\s+ice_udp")
RTC_ONDATA_RE = re.compile(
    r"totalelaps\s+(\d+).*?channel_ondata\s+(\d+).*?netreadpackagecount\s+(\d+)"
)
MEMORY_WATERLINE_RE = re.compile(
    r"memory waterline:.*?level=(\w+) internal=(\d+)K largest=(\d+)K "
    r"min=(\d+)K psram=(\d+)K largest=(\d+)K failures=(\d+)"
)
NUMBER_RE_TEMPLATE = r"(?:^|\s){key}=(\d+)"
FLOAT_RE_TEMPLATE = r"(?:^|\s){key}=([0-9.]+)"

CRITICAL_PATTERNS = {
    "sdio_dma_oom": re.compile(r"SDIO RX no DMA memory|copy_buff.*assert", re.IGNORECASE),
    "dma_escrow_reclaim": re.compile(r"dma escrow: action=reclaim-failed", re.IGNORECASE),
    "downlink_decode_no_mem": re.compile(
        r"H264 downlink decode lost sync: ret=ESP_ERR_NO_MEM", re.IGNORECASE
    ),
    "heartbeat_timeout": re.compile(r"TIRTC_E_HEARTBEAT_TIMEOUT|-40007"),
    "local_send_buffer_disconnect": re.compile(r"rtc send buffer stale:"),
    "panic": re.compile(r"Guru Meditation|assert failed|abort\(\)|Backtrace:", re.IGNORECASE),
    "task_watchdog": re.compile(r"Task watchdog got triggered", re.IGNORECASE),
    "invalid_handle": re.compile(r"TIRTC_E_INVALID_HANDLE|-40002"),
}


def number(text: str, key: str, default: int = 0) -> int:
    match = re.search(NUMBER_RE_TEMPLATE.format(key=re.escape(key)), text)
    return int(match.group(1)) if match else default


def decimal(text: str, key: str, default: float = 0.0) -> float:
    match = re.search(FLOAT_RE_TEMPLATE.format(key=re.escape(key)), text)
    return float(match.group(1)) if match else default


@dataclass
class SampleSet:
    camera: list[dict[str, float | int]] = field(default_factory=list)
    tx: list[dict[str, float | int]] = field(default_factory=list)
    downlink: list[dict[str, float | int]] = field(default_factory=list)
    active_runtime: list[dict[str, float | int]] = field(default_factory=list)
    close_snapshots: list[dict[str, int]] = field(default_factory=list)
    transport: list[dict[str, int]] = field(default_factory=list)
    rtc_poll_stalls_ms: list[int] = field(default_factory=list)
    rtc_ondata: list[dict[str, int]] = field(default_factory=list)
    memory_waterlines: list[dict[str, int | str]] = field(default_factory=list)
    first_timestamp: str | None = None
    last_timestamp: str | None = None
    first_uptime_ms: int | None = None
    last_uptime_ms: int | None = None
    first_upstream: bool = False
    subscribed: bool = False
    critical: dict[str, int] = field(default_factory=lambda: {key: 0 for key in CRITICAL_PATTERNS})


def parse_log(path: Path) -> SampleSet:
    samples = SampleSet()
    with path.open("r", encoding="utf-8-sig", errors="replace") as handle:
        for raw_line in handle:
            line = raw_line.strip()
            timestamp = TIMESTAMP_RE.match(line)
            if timestamp:
                samples.first_timestamp = samples.first_timestamp or timestamp.group(1)
                samples.last_timestamp = timestamp.group(1)
            uptime = UPTIME_RE.search(line)
            if uptime:
                uptime_ms = int(uptime.group(1))
                samples.first_uptime_ms = samples.first_uptime_ms or uptime_ms
                samples.last_uptime_ms = uptime_ms

            samples.first_upstream |= "camera pipeline first upstream frame" in line
            samples.subscribed |= "remote local video subscription" in line and "subscribed=1" in line
            for name, pattern in CRITICAL_PATTERNS.items():
                if pattern.search(line):
                    samples.critical[name] += 1

            compact_camera = COMPACT_CAMERA_RE.search(line)
            compact_tx = COMPACT_TX_RE.search(line)
            compact_downlink = COMPACT_DOWNLINK_RE.search(line)
            compact_downlink_parts = COMPACT_DOWNLINK_PARTS_RE.search(line)
            transport = TGTRP_DIAG_RE.search(line)
            rtc_poll_stall = RTC_POLL_STALL_RE.search(line)
            rtc_ondata = RTC_ONDATA_RE.search(line)
            memory_waterline = MEMORY_WATERLINE_RE.search(line)

            if compact_camera:
                compact_motion = COMPACT_CAMERA_MOTION_RE.search(line)
                camera_drops = [int(compact_camera.group(i)) for i in range(13, 17)]
                camera_failures = [int(compact_camera.group(i)) for i in range(18, 21)]
                samples.camera.append(
                    {
                        "width": int(compact_camera.group(1)),
                        "height": int(compact_camera.group(2)),
                        "target_fps": int(compact_camera.group(3)),
                        "fps": float(compact_camera.group(4)),
                        "bitrate": int(compact_camera.group(5)),
                        "encoded": 0,
                        "upstream": 0,
                        "drop": sum(camera_drops),
                        "cap_fail": camera_failures[0],
                        "convert_fail": camera_failures[1],
                        "enc_fail": camera_failures[2],
                        "max_gap_us": int(compact_camera.group(7)) * 1000,
                        "avg_capture_us": int(compact_camera.group(8)),
                        "avg_scale_us": int(compact_camera.group(9)),
                        "avg_encode_us": int(compact_camera.group(10)),
                        "avg_callback_us": int(compact_camera.group(11)),
                        "avg_loop_us": int(compact_camera.group(12)),
                        "dma_largest": 0,
                        "payload_avg": 0,
                        "payload_max": 0,
                        "luma_src_delta": float(compact_motion.group(1)) if compact_motion else 0.0,
                        "luma_src_transitions": int(compact_motion.group(4)) if compact_motion else 0,
                        "luma_enc_delta": float(compact_motion.group(2)) if compact_motion else 0.0,
                        "luma_enc_transitions": int(compact_motion.group(6)) if compact_motion else 0,
                        "compact": 1,
                    }
                )
            elif "camera pipeline stats:" in line:
                target = TARGET_RE.search(line)
                payload = PAYLOAD_RE.search(line)
                source_change = LUMA_SOURCE_CHANGE_RE.search(line)
                encoder_change = LUMA_ENCODER_CHANGE_RE.search(line)
                samples.camera.append(
                    {
                        "width": int(target.group(1)) if target else 0,
                        "height": int(target.group(2)) if target else 0,
                        "target_fps": int(target.group(3)) if target else 0,
                        "fps": decimal(line, "fps"),
                        "bitrate": number(line, "bitrate"),
                        "encoded": number(line, "encoded"),
                        "upstream": number(line, "upstream"),
                        "drop": number(line, "drop"),
                        "cap_fail": number(line, "cap_fail"),
                        "enc_fail": number(line, "enc_fail"),
                        "max_gap_us": number(line, "max_gap_us"),
                        "avg_capture_us": number(line, "avg_cap_us"),
                        "avg_scale_us": number(line, "avg_scale_us"),
                        "avg_encode_us": number(line, "avg_enc_us"),
                        "avg_callback_us": number(line, "avg_cb_us"),
                        "avg_loop_us": number(line, "avg_loop_us"),
                        "dma_largest": number(line, "dma_largest"),
                        "payload_avg": int(payload.group(2)) if payload else 0,
                        "payload_max": int(payload.group(3)) if payload else 0,
                        "luma_src_delta": decimal(line, "luma_src_delta"),
                        "luma_src_transitions": int(source_change.group(2)) if source_change else 0,
                        "luma_enc_delta": decimal(line, "luma_enc_delta"),
                        "luma_enc_transitions": int(encoder_change.group(2)) if encoder_change else 0,
                    }
                )
            elif compact_tx:
                samples.tx.append(
                    {
                        "sent": int(compact_tx.group(3)),
                        "fps": float(compact_tx.group(1)),
                        "bitrate": int(compact_tx.group(2)),
                        "fail": int(compact_tx.group(4)),
                        "busy": int(compact_tx.group(5)),
                        "defer": int(compact_tx.group(6)),
                        "throttle": int(compact_tx.group(7)),
                        "stale": int(compact_tx.group(8)),
                        "queue_depth": int(compact_tx.group(9)),
                        "free_slots": int(compact_tx.group(10)),
                        "send_buffer": int(compact_tx.group(11)),
                        "send_buffer_peak": int(compact_tx.group(12)),
                        "rx_callbacks": int(compact_tx.group(13)),
                        "rx_ok": int(compact_tx.group(14)),
                        "rx_fail": int(compact_tx.group(15)),
                        "rx_gap_ms": int(compact_tx.group(16)),
                    }
                )
            elif "local video tx stats:" in line:
                samples.tx.append(
                    {
                        "sent": number(line, "sent"),
                        "fps": decimal(line, "fps"),
                        "bitrate": number(line, "bitrate"),
                        "fail": number(line, "fail"),
                        "busy": number(line, "busy"),
                        "stale": number(line, "stale"),
                    }
                )
            elif compact_downlink:
                samples.downlink.append(
                    {
                        "width": int(compact_downlink.group(1)),
                        "height": int(compact_downlink.group(2)),
                        "rx_fps": float(compact_downlink.group(3)),
                        "rx_bitrate": int(compact_downlink.group(4)),
                        "queued_fps": float(compact_downlink.group(5)),
                        "decoded_fps": float(compact_downlink.group(6)),
                        "converted_fps": float(compact_downlink.group(7)),
                        "presented_fps": float(compact_downlink.group(8)),
                        "input_drops": int(compact_downlink.group(9)),
                        "display_drops": int(compact_downlink.group(10)),
                        "decode_failures": int(compact_downlink.group(11)),
                        "convert_failures": int(compact_downlink.group(12)),
                        "queue_age_max_ms": int(compact_downlink.group(14)),
                        "input_depth": int(compact_downlink.group(15)),
                        "decoded_depth": int(compact_downlink.group(16)),
                        "output_depth": int(compact_downlink.group(17)),
                        "target_depth": int(compact_downlink.group(18)),
                        "playout_interval_ms": int(compact_downlink.group(19)),
                        "decode_ms": int(compact_downlink.group(20)),
                        "convert_ms": int(compact_downlink.group(21)),
                        "ppa_ms": int(compact_downlink.group(22)),
                        "pack_ms": int(compact_downlink_parts.group(1))
                        if compact_downlink_parts
                        else 0,
                        "swap_ms": int(compact_downlink_parts.group(2))
                        if compact_downlink_parts
                        else 0,
                        "ui_copy_ms": int(compact_downlink_parts.group(3))
                        if compact_downlink_parts
                        else 0,
                        "decode_max_ms": int(compact_downlink_parts.group(4))
                        if compact_downlink_parts
                        else 0,
                        "convert_max_ms": int(compact_downlink_parts.group(5))
                        if compact_downlink_parts
                        else 0,
                        "ppa_max_ms": int(compact_downlink_parts.group(6))
                        if compact_downlink_parts
                        else 0,
                        "key_decode_ms": int(compact_downlink.group(23) or 0),
                        "delta_decode_ms": int(compact_downlink.group(24) or 0),
                        "receive_gap_ms": int(compact_downlink.group(25)),
                        "present_gap_ms": int(compact_downlink.group(26)),
                        "decoder_creations": 0,
                        # Compact firmware prints reset=restart/discontinuity
                        # and, on current builds, a separate overflow counter.
                        "decoder_restarts": int(compact_downlink.group(29)),
                        "resets": int(compact_downlink.group(30)),
                        "overflows": int(compact_downlink.group(31) or 0),
                    }
                )
            elif "H264 downlink stats:" in line:
                downlink = DOWNLINK_RE.search(line)
                if downlink:
                    samples.downlink.append(
                        {
                            "rx_fps": float(downlink.group(1)),
                            "rx_bitrate": int(downlink.group(2)),
                            "queued_fps": float(downlink.group(3)),
                            "decoded_fps": float(downlink.group(4)),
                            "converted_fps": float(downlink.group(5)),
                            "presented_fps": float(downlink.group(6)),
                            "input_drops": int(downlink.group(7)),
                            "display_drops": int(downlink.group(8)),
                            "decode_failures": int(downlink.group(9)),
                            "convert_failures": int(downlink.group(10)),
                            "decoder_creations": int(downlink.group(11)),
                            "decoder_restarts": int(downlink.group(12)),
                            "resets": int(downlink.group(13)),
                            "overflows": int(downlink.group(14)),
                        }
                    )
            elif "runtime snapshot:" in line:
                active = ACTIVE_RUNTIME_RE.search(line)
                if active:
                    samples.active_runtime.append(
                        {
                            "width": int(active.group(1)),
                            "height": int(active.group(2)),
                            "fps": float(active.group(3)),
                            "bitrate": int(active.group(4)),
                            "dma_free": number(line, "dma_free"),
                            "dma_largest": number(line, "dma_largest"),
                            "internal_largest": number(line, "internal_largest"),
                            "psram_free": number(line, "psram_free"),
                            "video_q": number(line, "video_q"),
                            "free": number(line, "free"),
                            "rtc_sendbuf": number(line, "rtc_sendbuf"),
                        }
                    )
            elif "rtc close snapshot:" in line:
                close = CLOSE_SNAPSHOT_RE.search(line)
                if close:
                    samples.close_snapshots.append(
                        {
                            "age_ms": int(close.group(1)),
                            "attempts": int(close.group(2)),
                            "failures": int(close.group(3)),
                            "video_frames": int(close.group(4)),
                            "video_kb": int(close.group(5)),
                            "audio_frames": int(close.group(6)),
                            "audio_kb": int(close.group(7)),
                            "send_buffer_used": int(close.group(8)),
                            "send_buffer_limit": int(close.group(9)),
                        }
                    )
            if transport:
                samples.transport.append(
                    {
                        "retransmits": int(transport.group(1)),
                        "lost": int(transport.group(2)),
                        "rx_stall_ms": int(transport.group(3)),
                        "rtt_ms": int(transport.group(4)),
                        "rto_ms": int(transport.group(5)),
                        "sendq_packets": int(transport.group(6)),
                        "sendq_bytes": int(transport.group(7)),
                        "pktbuf_packets": int(transport.group(8)),
                        "pktbuf_bytes": int(transport.group(9)),
                        "delivered": int(transport.group(10)),
                        "drop": int(transport.group(11)),
                    }
                )
            if rtc_poll_stall:
                samples.rtc_poll_stalls_ms.append(int(rtc_poll_stall.group(1)))
            if rtc_ondata:
                samples.rtc_ondata.append(
                    {
                        "total_ms": int(rtc_ondata.group(1)),
                        "channel_ondata_ms": int(rtc_ondata.group(2)),
                        "packets": int(rtc_ondata.group(3)),
                    }
                )
            if memory_waterline:
                samples.memory_waterlines.append(
                    {
                        "level": memory_waterline.group(1),
                        "internal_kb": int(memory_waterline.group(2)),
                        "internal_largest_kb": int(memory_waterline.group(3)),
                        "internal_min_kb": int(memory_waterline.group(4)),
                        "psram_kb": int(memory_waterline.group(5)),
                        "psram_largest_kb": int(memory_waterline.group(6)),
                        "failures": int(memory_waterline.group(7)),
                    }
                )
    return samples


def mean(items: list[dict[str, float | int]], key: str) -> float:
    values = [float(item[key]) for item in items]
    return statistics.fmean(values) if values else 0.0


def minimum(items: list[dict[str, float | int]], key: str) -> int:
    values = [int(item[key]) for item in items if int(item[key]) > 0]
    return min(values) if values else 0


def percentile(items: list[dict[str, float | int]], key: str, ratio: float) -> float:
    values = sorted(float(item[key]) for item in items)
    if not values:
        return 0.0
    position = max(0.0, min(1.0, ratio)) * (len(values) - 1)
    lower = int(position)
    upper = min(lower + 1, len(values) - 1)
    fraction = position - lower
    return values[lower] + (values[upper] - values[lower]) * fraction


@dataclass
class Check:
    level: str
    message: str


def evaluate(samples: SampleSet) -> tuple[str, list[Check]]:
    checks: list[Check] = []

    critical_total = sum(samples.critical.values())
    if critical_total:
        detail = ", ".join(f"{key}={value}" for key, value in samples.critical.items() if value)
        checks.append(Check("FAIL", f"critical runtime errors: {detail}"))
    else:
        checks.append(Check("PASS", "no DMA OOM, heartbeat timeout, invalid handle or panic detected"))

    if not samples.first_upstream:
        checks.append(Check("FAIL", "no first upstream H264 frame"))
    elif not samples.camera:
        close = samples.close_snapshots[-1] if samples.close_snapshots else None
        if close and close["video_frames"] > 0 and close["age_ms"] < 15_000:
            checks.append(
                Check(
                    "WARN",
                    "call ended before the 10-second camera statistics window; "
                    f"close snapshot still recorded {close['video_frames']} video frames",
                )
            )
        else:
            checks.append(Check("FAIL", "only startup frame observed; no sustained camera statistics"))
    else:
        checks.append(Check("PASS", f"camera produced {len(samples.camera)} sustained statistics samples"))

    if not samples.subscribed:
        checks.append(Check("WARN", "local video subscription confirmation was not found"))
    else:
        checks.append(Check("PASS", "local video subscription confirmed"))

    if samples.camera:
        target_fps = int(samples.camera[-1]["target_fps"])
        avg_fps = mean(samples.camera, "fps")
        p10_fps = percentile(samples.camera, "fps", 0.10)
        low_window_count = sum(
            1 for item in samples.camera if float(item["fps"]) < target_fps * 0.80
        )
        low_window_ratio = low_window_count / len(samples.camera)
        fps_ratio = avg_fps / target_fps if target_fps else 0.0
        if fps_ratio < 0.60 or low_window_ratio > 0.20:
            checks.append(
                Check(
                    "FAIL",
                    f"camera fps avg/p10={avg_fps:.1f}/{p10_fps:.1f} target={target_fps} "
                    f"low_windows={low_window_count}/{len(samples.camera)}",
                )
            )
        elif fps_ratio < 0.80 or low_window_count:
            checks.append(
                Check(
                    "WARN",
                    f"camera fps avg/p10={avg_fps:.1f}/{p10_fps:.1f} target={target_fps} "
                    f"low_windows={low_window_count}/{len(samples.camera)}",
                )
            )
        else:
            checks.append(Check("PASS", f"camera fps avg/p10={avg_fps:.1f}/{p10_fps:.1f}/{target_fps}"))

        upstream = sum(int(item["upstream"]) for item in samples.camera)
        dropped = sum(int(item["drop"]) for item in samples.camera)
        failures = sum(
            int(item["cap_fail"]) + int(item.get("convert_fail", 0)) + int(item["enc_fail"])
            for item in samples.camera
        )
        if failures:
            checks.append(Check("FAIL", f"camera capture/encode failures={failures}"))
        elif upstream + dropped:
            drop_ratio = dropped / (upstream + dropped)
            if drop_ratio <= 0.10:
                checks.append(Check("PASS", f"camera drop ratio {drop_ratio * 100:.1f}%"))
            elif drop_ratio <= 0.25:
                checks.append(Check("WARN", f"camera drop ratio {drop_ratio * 100:.1f}%"))
            else:
                checks.append(Check("FAIL", f"camera drop ratio {drop_ratio * 100:.1f}%"))
        elif dropped == 0:
            checks.append(Check("PASS", "camera compact windows reported no drops"))
        else:
            checks.append(Check("WARN", f"camera compact windows reported drops={dropped}"))

        max_gap_us = max(int(item["max_gap_us"]) for item in samples.camera)
        if max_gap_us <= 150_000:
            checks.append(Check("PASS", f"maximum frame gap {max_gap_us / 1000:.1f}ms"))
        elif max_gap_us <= 300_000:
            checks.append(Check("WARN", f"maximum frame gap {max_gap_us / 1000:.1f}ms"))
        else:
            checks.append(Check("FAIL", f"maximum frame gap {max_gap_us / 1000:.1f}ms"))

        avg_bitrate = mean(samples.camera, "bitrate")
        avg_payload = mean(samples.camera, "payload_avg")
        max_payload = max(int(item["payload_max"]) for item in samples.camera)
        has_payload_metrics = any(int(item["payload_max"]) > 0 for item in samples.camera)
        if has_payload_metrics and avg_bitrate <= 32.0 and avg_payload <= 128.0 and max_payload <= 2048:
            checks.append(
                Check(
                    "FAIL",
                    "H264 output is near-empty: "
                    f"average={avg_payload:.0f}B, max={max_payload}B, bitrate={avg_bitrate:.0f}kbps",
                )
            )

        luma_samples = [
            item
            for item in samples.camera
            if int(item["luma_src_transitions"]) > 0 and int(item["luma_enc_transitions"]) > 0
        ]
        if luma_samples:
            source_delta = mean(luma_samples, "luma_src_delta")
            encoder_delta = mean(luma_samples, "luma_enc_delta")
            if source_delta >= 1.0 and encoder_delta < max(0.2, source_delta * 0.10):
                checks.append(
                    Check(
                        "FAIL",
                        f"PPA output appears frozen: source luma delta={source_delta:.1f}, "
                        f"encoder-input delta={encoder_delta:.1f}",
                    )
                )
            elif encoder_delta >= 1.0 and avg_bitrate <= 32.0:
                checks.append(
                    Check(
                        "FAIL",
                        f"H264 encoder emitted near-empty frames despite changing input "
                        f"(luma delta={encoder_delta:.1f})",
                    )
                )
            else:
                checks.append(
                    Check(
                        "PASS",
                        f"luma probe source/encoder-input delta={source_delta:.1f}/{encoder_delta:.1f}",
                    )
                )

    if samples.tx:
        sent = sum(int(item["sent"]) for item in samples.tx)
        failed = sum(int(item["fail"]) for item in samples.tx)
        tx_ratio = failed / (sent + failed) if sent + failed else 1.0
        avg_tx_fps = mean(samples.tx, "fps")
        if tx_ratio <= 0.05:
            checks.append(Check("PASS", f"TiRTC video TX failure ratio {tx_ratio * 100:.1f}%"))
        elif tx_ratio <= 0.15:
            checks.append(Check("WARN", f"TiRTC video TX failure ratio {tx_ratio * 100:.1f}%"))
        else:
            checks.append(Check("FAIL", f"TiRTC video TX failure ratio {tx_ratio * 100:.1f}%"))
        target_fps = int(samples.camera[-1]["target_fps"]) if samples.camera else 15
        tx_ratio_to_target = avg_tx_fps / target_fps if target_fps else 0.0
        if tx_ratio_to_target < 0.67:
            checks.append(Check("FAIL", f"TiRTC average video TX fps {avg_tx_fps:.1f}"))
        elif tx_ratio_to_target < 0.90:
            checks.append(Check("WARN", f"TiRTC average video TX fps {avg_tx_fps:.1f}"))
        else:
            checks.append(
                Check("PASS", f"TiRTC average video TX fps {avg_tx_fps:.1f}/{target_fps}")
            )
    elif samples.close_snapshots:
        close = samples.close_snapshots[-1]
        duration_s = max(close["age_ms"] / 1000.0, 0.001)
        close_fps = close["video_frames"] / duration_s
        close_bitrate_kbps = close["video_kb"] * 8.0 / duration_s
        level = "PASS" if close_fps >= 15.0 else "WARN" if close_fps >= 10.0 else "FAIL"
        checks.append(
            Check(
                level,
                "short-session close snapshot: "
                f"video={close['video_frames']} frames, {close_fps:.1f}fps, "
                f"{close_bitrate_kbps:.0f}kbps",
            )
        )
    else:
        checks.append(Check("FAIL", "no periodic TiRTC video TX statistics or close snapshot"))

    active_downlink = [item for item in samples.downlink if float(item["rx_fps"]) > 0.0]
    if active_downlink:
        rx_fps = mean(active_downlink, "rx_fps")
        decoded_fps = mean(active_downlink, "decoded_fps")
        presented_fps = mean(active_downlink, "presented_fps")
        presented_p10_fps = percentile(active_downlink, "presented_fps", 0.10)
        input_drops = sum(int(item["input_drops"]) for item in active_downlink)
        decode_failures = sum(int(item["decode_failures"]) for item in active_downlink)
        restarts = sum(int(item["decoder_restarts"]) for item in active_downlink)
        resets = sum(int(item["resets"]) for item in active_downlink)
        overflows = sum(int(item["overflows"]) for item in active_downlink)
        decode_ratio = decoded_fps / rx_fps if rx_fps else 0.0
        present_ratio = presented_fps / rx_fps if rx_fps else 0.0

        if decode_failures or decode_ratio < 0.60:
            checks.append(
                Check(
                    "FAIL",
                    f"H264 downlink decode {decoded_fps:.1f}/{rx_fps:.1f}fps "
                    f"failures={decode_failures} restarts={restarts} resets={resets} "
                    f"overflows={overflows} input_drops={input_drops}",
                )
            )
        elif decode_ratio < 0.85 or restarts or overflows:
            checks.append(
                Check(
                    "WARN",
                    f"H264 downlink decode {decoded_fps:.1f}/{rx_fps:.1f}fps "
                    f"restarts={restarts} overflows={overflows} input_drops={input_drops}",
                )
            )
        else:
            checks.append(Check("PASS", f"H264 downlink decode {decoded_fps:.1f}/{rx_fps:.1f}fps"))

        if present_ratio < 0.50:
            checks.append(Check("FAIL", f"LCD presentation avg/p10={presented_fps:.1f}/{presented_p10_fps:.1f}, rx={rx_fps:.1f}fps"))
        elif present_ratio < 0.75 or presented_p10_fps < rx_fps * 0.60:
            checks.append(Check("WARN", f"LCD presentation avg/p10={presented_fps:.1f}/{presented_p10_fps:.1f}, rx={rx_fps:.1f}fps"))
        else:
            checks.append(Check("PASS", f"LCD presentation avg/p10={presented_fps:.1f}/{presented_p10_fps:.1f}, rx={rx_fps:.1f}fps"))

    if samples.transport:
        max_lost = max(int(item["lost"]) for item in samples.transport)
        max_rx_stall = max(int(item["rx_stall_ms"]) for item in samples.transport)
        max_rtt = max(int(item["rtt_ms"]) for item in samples.transport)
        max_sendq = max(int(item["sendq_packets"]) for item in samples.transport)
        max_pktbuf = max(int(item["pktbuf_packets"]) for item in samples.transport)
        if max_rx_stall > 200 or max_sendq > 64 or max_pktbuf > 64:
            checks.append(
                Check(
                    "FAIL",
                    f"TGTRP pressure lost={max_lost} rx_stall={max_rx_stall}ms "
                    f"sendq={max_sendq} pktbuf={max_pktbuf} rtt={max_rtt}ms",
                )
            )
        elif max_lost > 0 or max_rx_stall > 20 or max_rtt > 200:
            checks.append(
                Check(
                    "WARN",
                    f"TGTRP transient lost={max_lost} rx_stall={max_rx_stall}ms "
                    f"sendq={max_sendq} pktbuf={max_pktbuf} rtt={max_rtt}ms",
                )
            )
        else:
            checks.append(
                Check(
                    "PASS",
                    f"TGTRP queues clear, max_rtt={max_rtt}ms max_rx_stall={max_rx_stall}ms",
                )
            )

    if samples.rtc_poll_stalls_ms:
        max_poll_ms = max(samples.rtc_poll_stalls_ms)
        max_ondata_ms = max(
            (int(item["channel_ondata_ms"]) for item in samples.rtc_ondata),
            default=0,
        )
        if max_poll_ms > 500:
            checks.append(
                Check("FAIL", f"RTC socket processing stall {max_poll_ms}ms, channel_ondata={max_ondata_ms}ms")
            )
        elif max_poll_ms > 100:
            checks.append(
                Check("WARN", f"RTC socket processing stall {max_poll_ms}ms, channel_ondata={max_ondata_ms}ms")
            )
        else:
            checks.append(Check("PASS", f"RTC socket processing max {max_poll_ms}ms"))

    if samples.memory_waterlines:
        min_internal_kb = min(int(item["internal_kb"]) for item in samples.memory_waterlines)
        min_largest_kb = min(int(item["internal_largest_kb"]) for item in samples.memory_waterlines)
        allocation_failures = max(int(item["failures"]) for item in samples.memory_waterlines)
        if allocation_failures:
            checks.append(Check("FAIL", f"memory allocation failures={allocation_failures}"))
        elif min_largest_kb < 16:
            checks.append(
                Check("WARN", f"internal memory pressure free={min_internal_kb}K largest={min_largest_kb}K")
            )
        else:
            checks.append(
                Check("PASS", f"internal memory waterline free={min_internal_kb}K largest={min_largest_kb}K")
            )

    dma_largest = minimum(samples.active_runtime, "dma_largest") or minimum(samples.camera, "dma_largest")
    if dma_largest >= 16 * 1024:
        checks.append(Check("PASS", f"minimum largest DMA block {dma_largest} bytes"))
    elif dma_largest >= 8 * 1024:
        checks.append(Check("WARN", f"minimum largest DMA block {dma_largest} bytes"))
    elif dma_largest > 0:
        checks.append(Check("FAIL", f"minimum largest DMA block {dma_largest} bytes"))
    else:
        checks.append(Check("WARN", "largest DMA block is not present in compact logs"))

    levels = {check.level for check in checks}
    result = "FAIL" if "FAIL" in levels else "WARN" if "WARN" in levels else "PASS"
    return result, checks


def print_report(path: Path, samples: SampleSet, result: str, checks: list[Check]) -> None:
    print("Media performance report")
    print(f"file: {path}")
    if samples.first_timestamp or samples.last_timestamp:
        window = f"{samples.first_timestamp or '-'} -> {samples.last_timestamp or '-'}"
    elif samples.first_uptime_ms is not None and samples.last_uptime_ms is not None:
        window = f"uptime {samples.first_uptime_ms / 1000:.1f}s -> {samples.last_uptime_ms / 1000:.1f}s"
    else:
        window = "-"
    print(f"window: {window}")
    print(f"result: {result}")
    if samples.camera:
        last = samples.camera[-1]
        print(
            "camera: "
            f"{int(last['width'])}x{int(last['height'])}@{int(last['target_fps'])} "
            f"avg_fps={mean(samples.camera, 'fps'):.1f} "
            f"p10/min_fps={percentile(samples.camera, 'fps', 0.10):.1f}/"
            f"{percentile(samples.camera, 'fps', 0.0):.1f} "
            f"avg_bitrate={mean(samples.camera, 'bitrate'):.0f}kbps samples={len(samples.camera)}"
        )
        print(
            "camera_stages: "
            f"avg_ms=capture:{mean(samples.camera, 'avg_capture_us') / 1000.0:.1f} "
            f"scale:{mean(samples.camera, 'avg_scale_us') / 1000.0:.1f} "
            f"encode:{mean(samples.camera, 'avg_encode_us') / 1000.0:.1f} "
            f"callback:{mean(samples.camera, 'avg_callback_us') / 1000.0:.1f} "
            f"loop:{mean(samples.camera, 'avg_loop_us') / 1000.0:.1f} "
            f"p90_ms=scale:{percentile(samples.camera, 'avg_scale_us', 0.90) / 1000.0:.1f} "
            f"encode:{percentile(samples.camera, 'avg_encode_us', 0.90) / 1000.0:.1f} "
            f"loop:{percentile(samples.camera, 'avg_loop_us', 0.90) / 1000.0:.1f}"
        )
        luma_samples = [
            item
            for item in samples.camera
            if int(item["luma_src_transitions"]) > 0 and int(item["luma_enc_transitions"]) > 0
        ]
        if luma_samples:
            print(
                "luma_probe: "
                f"source_delta={mean(luma_samples, 'luma_src_delta'):.1f} "
                f"encoder_input_delta={mean(luma_samples, 'luma_enc_delta'):.1f}"
            )
    if samples.tx:
        print(
            "tirtc_tx: "
            f"avg_fps={mean(samples.tx, 'fps'):.1f} "
            f"p10/min_fps={percentile(samples.tx, 'fps', 0.10):.1f}/"
            f"{percentile(samples.tx, 'fps', 0.0):.1f} "
            f"avg_bitrate={mean(samples.tx, 'bitrate'):.0f}kbps samples={len(samples.tx)}"
        )
    active_downlink = [item for item in samples.downlink if float(item["rx_fps"]) > 0.0]
    if active_downlink:
        print(
            "downlink: "
            f"rx={mean(active_downlink, 'rx_fps'):.1f}fps "
            f"decoded={mean(active_downlink, 'decoded_fps'):.1f}fps "
            f"presented={mean(active_downlink, 'presented_fps'):.1f}fps "
            f"presented_p10/min={percentile(active_downlink, 'presented_fps', 0.10):.1f}/"
            f"{percentile(active_downlink, 'presented_fps', 0.0):.1f}fps "
            f"samples={len(active_downlink)}"
        )
        if any(int(item.get("convert_max_ms", 0)) > 0 for item in active_downlink):
            print(
                "downlink_stages: "
                f"avg_ms=decode:{mean(active_downlink, 'decode_ms'):.1f} "
                f"convert:{mean(active_downlink, 'convert_ms'):.1f} "
                f"pack:{mean(active_downlink, 'pack_ms'):.1f} "
                f"ppa:{mean(active_downlink, 'ppa_ms'):.1f} "
                f"swap:{mean(active_downlink, 'swap_ms'):.1f} "
                f"ui:{mean(active_downlink, 'ui_copy_ms'):.1f} "
                f"max_ms=decode:{max(int(item['decode_max_ms']) for item in active_downlink)} "
                f"convert:{max(int(item['convert_max_ms']) for item in active_downlink)} "
                f"ppa:{max(int(item['ppa_max_ms']) for item in active_downlink)}"
            )
    if samples.close_snapshots:
        close = samples.close_snapshots[-1]
        duration_s = max(close["age_ms"] / 1000.0, 0.001)
        print(
            "close_snapshot: "
            f"age={duration_s:.1f}s video={close['video_frames']} "
            f"audio={close['audio_frames']} attempts={close['attempts']} "
            f"failures={close['failures']} "
            f"sdk_buf={close['send_buffer_used']}/{close['send_buffer_limit']}"
        )
    if samples.transport:
        print(
            "transport: "
            f"samples={len(samples.transport)} "
            f"max_lost={max(int(item['lost']) for item in samples.transport)} "
            f"max_rx_stall={max(int(item['rx_stall_ms']) for item in samples.transport)}ms "
            f"max_rtt={max(int(item['rtt_ms']) for item in samples.transport)}ms"
        )
    if samples.rtc_poll_stalls_ms:
        print(
            "rtc_poll: "
            f"samples={len(samples.rtc_poll_stalls_ms)} "
            f"max={max(samples.rtc_poll_stalls_ms)}ms"
        )
    print("checks:")
    for check in checks:
        print(f"  [{check.level}] {check.message}")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="UTF-8 serial log file")
    args = parser.parse_args()

    if not args.log.is_file():
        parser.error(f"log file not found: {args.log}")
    samples = parse_log(args.log)
    result, checks = evaluate(samples)
    print_report(args.log, samples, result, checks)
    return {"PASS": 0, "WARN": 1, "FAIL": 2}[result]


if __name__ == "__main__":
    sys.exit(main())
