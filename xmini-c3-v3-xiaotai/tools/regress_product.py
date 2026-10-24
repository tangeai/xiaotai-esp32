#!/usr/bin/env python3
"""Two explicitly identified C3 devices: asynchronous product AT regression.

No flashing, credentials, account binding or authorization changes. GROUP
mutations require an explicit option and an initially unassigned pair. Raw
serial logs and media remain in a new caller-supplied PRIVATE directory.
"""
import argparse
import hashlib
import json
import math
import re
import struct
import time
import traceback
import wave
from pathlib import Path

ACOUSTIC_WINDOW_START_FRAME = 10
ACOUSTIC_WINDOW_END_FRAME = 50


class FatalDeviceError(RuntimeError):
    def __init__(self, label, evidence):
        self.label, self.evidence = label, evidence
        super().__init__(f"{label}: fatal device event: {evidence['markers'][0]['line']}")


def fatal_markers(lines, start):
    patterns = ("Guru Meditation", "assert failed", "CORRUPT HEAP", "Stack protection", "abort()", "rst:0x")
    return [{"line_index": index, "line": line}
            for index, line in enumerate(lines[start:], start) if any(marker in line for marker in patterns)]


def tone_summary(frames):
    longest = consecutive = 0
    for frame in frames:
        consecutive = consecutive + 1 if frame["tone"] else 0
        longest = max(longest, consecutive)
    count = sum(frame["tone"] for frame in frames)
    return {"tone_frames": count, "longest_tone_run": longest, "passed": count >= 10 and longest >= 8}


def acoustic_comparison(recording, baseline):
    start, end = ACOUSTIC_WINDOW_START_FRAME, ACOUSTIC_WINDOW_END_FRAME
    assert len(recording["frames"]) == len(baseline["frames"]) == end, "Expected two complete one-second captures"
    frames = recording["frames"][start:end]
    background = sorted(frame["rms_400"] for frame in baseline["frames"][start:end])
    foreground = sorted(frame["rms_400"] for frame in frames if frame["tone"])
    background_p90 = background[int(len(background) * 0.9)]
    foreground_median = foreground[len(foreground) // 2] if foreground else 0
    contrast = foreground_median / max(1, background_p90)
    summary = tone_summary(frames)
    return {"window": {"start_frame": start, "end_frame_exclusive": end,
                       "start_ms": start * 20, "end_ms": end * 20,
                       "definition": "Fixed recorder-relative window, identical for baseline and stimulus; not selected by detected tone"},
            "baseline_p90_rms_400": background_p90, "foreground_median_rms_400": foreground_median,
            "relative_400_rms_gain": round(contrast, 3), "tone": summary,
            "passed": summary["passed"] and contrast >= 3,
            "criterion": "Original acoustic per-frame DFT thresholds; >=10 tone frames and >=8 consecutive in the fixed window; contrast>=3"}


def controls_saved(state, original=None):
    audio, settings = state["AUDIO"], state["SETTINGS"]
    return (number(audio, "busy") == number(audio, "device_open") == 0 and
            number(settings, "loaded") == number(settings, "saved") == 1 and
            number(settings, "pending") == number(settings, "error") == 0 and
            (original is None or (number(audio, "volume") == original["volume"] and
                                  number(audio, "gain") == original["gain"])))


def retired(state):
    return (number(state["SESSION"], "owner") == number(state["SESSION"], "media_active") == 0 and
            number(state["SESSION"], "phase") in (0, 8) and
            number(state["RTC"], "initialized") == number(state["RTC"], "online") == 1 and
            number(state["MEDIA"], "events") == number(state["MEDIA"], "pending") == 0 and
            all(number(state["RTC"], key) == 0 for key in ("connecting", "connected", "closing", "stopping", "sdk_pending")))


def fields(lines):
    result = {}
    for line in lines:
        match = re.match(r"^\+([A-Z_]+):", line)
        if match:
            result[match[1]] = dict(re.findall(r"(\w+)=([^ ]*)", line))
    return result


def number(table, key):
    if key not in table:
        raise AssertionError(f"Missing field {key}: {table}")
    return int(table[key])


def alaw_decode(packet):
    samples = []
    for byte in packet:
        value = byte ^ 0x55
        segment = (value >> 4) & 7
        magnitude = (value & 15) << 4
        magnitude += 8 if segment == 0 else 0x108
        if segment > 1:
            magnitude <<= segment - 1
        samples.append(magnitude if value & 0x80 else -magnitude)
    return samples


def tone_energy(packet, acoustic=False):
    """Per-20ms exact-bin DFT; no optional numerical dependencies."""
    samples = alaw_decode(packet)
    frames = []
    for offset in range(0, len(samples), 160):
        frame = samples[offset:offset + 160]
        if len(frame) != 160:
            raise AssertionError("Partial PCMA frame")
        mean = sum(frame) / 160
        centered = [sample - mean for sample in frame]
        power = sum(sample * sample for sample in centered) / 160
        energies = {}
        for frequency in (300, 350, 400, 450, 500):
            real = sum(sample * math.cos(2 * math.pi * frequency * i / 8000) for i, sample in enumerate(centered))
            imaginary = sum(sample * math.sin(2 * math.pi * frequency * i / 8000) for i, sample in enumerate(centered))
            energies[frequency] = 2 * (real * real + imaginary * imaginary) / (160 * 160)
        ratio = energies[400] / max(power, 1)
        separation = energies[400] / max(1, *(value for frequency, value in energies.items() if frequency != 400))
        frames.append({"rms": round(math.sqrt(power), 2), "rms_400": round(math.sqrt(energies[400]), 2), "ratio_400": round(ratio, 4),
                       "neighbor_separation": round(separation, 3),
                       "tone": math.sqrt(energies[400]) >= (30 if acoustic else 100) and
                       ratio >= (0.12 if acoustic else 0.45) and separation >= 4})
    return {"frames": frames, **tone_summary(frames),
            "criterion": f"400Hz RMS>={30 if acoustic else 100}; >={12 if acoustic else 45}% frame power; >=4x adjacent bins; >=10 frames and >=8 consecutive"}


class Peer:
    def __init__(self, label, port, mac, identity, elf, output):
        from regress_at import Device
        self.label, self.mac, self.identity, self.elf = label, mac.lower(), identity, elf.lower()
        output.mkdir()
        self.device = Device(port, output)
        self.verified = False
        self.verified_line_index = 0
        self.sof_baseline = None
        self.fatal = None

    def check_fatal(self, raise_error=True):
        if self.verified:
            markers = fatal_markers(self.device.lines, self.verified_line_index)
            if markers:
                self.fatal = {"markers": markers[:16], "capture_restarted": any("rst:0x" in item["line"] for item in markers),
                              "interpretation": "A reset is not SDK retirement or proof of resource recovery"}
        if self.fatal and raise_error:
            raise FatalDeviceError(self.label, self.fatal)

    def command(self, text, timeout=3, diagnostic=False):
        if diagnostic:
            assert text in ("AT+RTC", "AT+STATUS", "AT+MEMTRACE", "AT+CONSOLE", "AT+VERSION", "AT+PRODUCT")
        else:
            self.check_fatal()
        start = len(self.device.lines)
        self.device.commands += 1
        self.device.send(text.encode("ascii") + b"\n")
        try:
            return self.device.response(start, timeout=timeout)
        finally:
            self.check_fatal(raise_error=not diagnostic)

    def diagnostic(self):
        evidence = {}
        for command in ("AT+VERSION", "AT+RTC", "AT+STATUS", "AT+MEMTRACE", "AT+CONSOLE"):
            try:
                evidence[command] = self.command(command, timeout=2, diagnostic=True)
            except Exception as error:
                evidence[command] = {"failure": repr(error)}
        self.check_fatal(raise_error=False)
        evidence["fatal"] = self.fatal
        return evidence

    def rtc(self):
        result = fields(self.command("AT+RTC"))
        assert {"RTC", "MEDIA", "SESSION", "GROUP"} <= result.keys(), result
        return result

    def audio(self):
        result = fields(self.command("AT+STATUS"))
        assert {"AUDIO", "HEAP", "STREAM"} <= result.keys(), result
        assert number(result["HEAP"], "integrity") == 1, result
        assert number(result["AUDIO"], "ready") == 1, result
        return result

    def snapshot(self):
        return {"rtc": self.rtc(), "audio": self.audio()}


class Regression:
    def __init__(self, peers, args):
        self.peers, self.args = peers, args
        self.stages, self.room, self.room_id = [], None, None
        self.acoustic_controls = {}
        self.stage_directory = args.output / "stages"
        self.stage_directory.mkdir()
        self.media_directory = args.output / "media"
        self.media_directory.mkdir()

    def stage(self, name, function, propagate_fatal=True):
        record = {"name": name, "passed": False, "started_at_unix": time.time()}
        started = time.monotonic()
        fatal = None
        try:
            record["evidence"] = function()
            record["passed"] = True
        except Exception as error:
            if isinstance(error, FatalDeviceError):
                fatal = error
            record["failure"] = f"{type(error).__name__}: {str(error)[:240]}"
            record["failure_details"] = repr(error)
            record["traceback"] = traceback.format_exc()
            record["diagnostic"] = {}
            for peer in self.peers:
                if peer.verified:
                    try:
                        record["diagnostic"][peer.label] = peer.diagnostic()
                        if peer.fatal:
                            fatal = FatalDeviceError(peer.label, peer.fatal)
                    except Exception as diagnostic_error:
                        record["diagnostic"][peer.label] = {"failure": repr(diagnostic_error)}
        finally:
            record["seconds"] = round(time.monotonic() - started, 3)
            self.stages.append(record)
            path = self.stage_directory / f"{len(self.stages):02d}-{name}.json"
            path.write_text(json.dumps(record, ensure_ascii=False, indent=2), encoding="utf-8")
            print(f"{name}: {'PASS' if record['passed'] else 'FAIL'}", flush=True)
        if fatal and propagate_fatal:
            raise fatal
        return record["passed"]

    def wait(self, peers, predicate, timeout=35, audio=False, reject_errors=True):
        deadline = time.monotonic() + timeout
        history = []
        while time.monotonic() < deadline:
            states = {peer.label: peer.audio() if audio else peer.rtc() for peer in peers}
            history.append(states)
            if predicate(states):
                return {"final": states, "observations": history[-40:]}
            if reject_errors and not audio:
                failed = {label: state for label, state in states.items() if number(state["SESSION"], "phase") == 8}
                if failed:
                    raise AssertionError({"terminal_business_error": failed, "observations": history[-10:]})
            time.sleep(0.03)
        raise TimeoutError({"last_observations": history[-10:], "seconds": timeout})

    def ready(self):
        identities = {}
        for peer in self.peers:
            peer.device.port.open()
            peer.device.read(0.3)
            capture_start = len(peer.device.lines)
            version = peer.command("AT+VERSION")
            assert any("board=Xmini-C3-V3" in line and "mac=" + peer.mac in line for line in version), version
            assert any("elf_sha256=" + peer.elf in line and "opus=0" in line and "video=0" in line and "rtc=1" in line for line in version), version
            product = fields(peer.command("AT+PRODUCT"))
            assert product["DEVICE"]["id"] == peer.identity, product
            peer.verified = True
            peer.verified_line_index = capture_start
            peer.check_fatal()
            identities[peer.label] = {"version": version, "product": product}
        assert len({peer.identity for peer in self.peers}) == len({peer.mac for peer in self.peers}) == 2
        state = self.wait(self.peers, lambda states: all(number(item["RTC"], "online") == 1 and
            number(item["SESSION"], "profile_ready") == 1 and
            number(item["SESSION"], "owner") == 0 and number(item["SESSION"], "phase") == 0 and
            all(number(item["RTC"], key) == 0 for key in ("connecting", "connected", "closing", "stopping", "sdk_pending"))
            for item in states.values()), timeout=60)
        contacts = {}
        for peer in self.peers:
            peer.command("AT+CONTACTS DEV 0")
            deadline = time.monotonic() + 20
            while time.monotonic() < deadline:
                lines = peer.command("AT+CONTACTS")
                page = fields(lines)["CONTACTS"]
                if number(page, "filter") == 1 and number(page, "loading") == 0:
                    assert number(page, "error") == 0, lines
                    other = next(item for item in self.peers if item is not peer)
                    assert any("id=" + other.identity in line for line in lines if line.startswith("+CONTACT:")), lines
                    contacts[peer.label] = lines
                    break
                time.sleep(0.05)
            else:
                raise TimeoutError("Authoritative DEV contact page did not load")
        for peer in self.peers:
            product = fields(peer.command("AT+PRODUCT"))
            assert product["DEVICE"]["id"] == peer.identity and product["DEVICE"]["platform_online"] == "1", product
            console = fields(peer.command("AT+CONSOLE"))
            peer.sof_baseline = number(console["SOF"], "disconnects")
        return {"identities": identities, "ready": state, "contacts": contacts,
                "baseline": {peer.label: peer.snapshot() for peer in self.peers}}

    def idle(self, peers=None):
        peers = peers or self.peers
        rtc = self.wait(peers, lambda states: all(number(item["SESSION"], "owner") == 0 and
            number(item["SESSION"], "phase") == 0 and number(item["SESSION"], "media_active") == 0 and
            number(item["RTC"], "connected") == 0 and number(item["RTC"], "connecting") == 0 and
            all(number(item["RTC"], key) == 0 for key in ("stopping", "closing", "sdk_pending"))
            for item in states.values()), timeout=25)
        audio = self.wait(peers, lambda states: all(number(item["AUDIO"], "busy") == 0 and
            number(item["AUDIO"], "device_open") == 0 for item in states.values()), timeout=10, audio=True)
        return {"rtc": rtc, "audio": audio}

    def cleanup(self):
        operations = []
        healthy = []
        fatal_targets = []
        for peer in self.peers:
            if not peer.verified:
                continue
            peer.check_fatal(raise_error=False)
            if peer.fatal:
                fatal_targets.append(peer.label)
                continue
            healthy.append(peer)
            state = peer.rtc()
            phase = number(state["SESSION"], "phase")
            if number(state["SESSION"], "owner") != 0 and phase != 7:
                operations.append({peer.label: peer.command("AT+CALL HANGUP")})
            retirement = self.wait((peer,), lambda states: retired(states[peer.label]), timeout=25, reject_errors=False)
            state = retirement["final"][peer.label]
            if number(state["SESSION"], "phase") == 8:
                self.wait((peer,), lambda states: number(states[peer.label]["AUDIO"], "busy") == 0 and
                    number(states[peer.label]["AUDIO"], "device_open") == 0, timeout=10, audio=True)
                operations.append({peer.label: {"retirement_before_dismiss": retirement,
                                               "dismiss": peer.command("AT+CALL DISMISS")}})
        return {"operations": operations, "idle": self.idle(healthy) if healthy else None,
                "fatal_targets_not_mutated": fatal_targets,
                "fatal_targets_retirement_proven": False if fatal_targets else None,
                "reset_is_not_retirement": True}

    def dial(self, caller, callee):
        caller.command("AT+CALL DEVICE " + callee.identity)
        ringing = self.wait((caller, callee), lambda states:
            number(states[caller.label]["SESSION"], "owner") == 2 and
            number(states[callee.label]["SESSION"], "owner") == 2 and
            number(states[callee.label]["SESSION"], "phase") == 3)
        for peer, expected in ((caller, callee.identity), (callee, caller.identity)):
            actual = ringing["final"][peer.label]["SESSION"]["peer"]
            assert actual == expected, {"device": peer.label, "expected_peer": expected, "actual": actual}
        return ringing

    def active(self, peers, owner):
        rtc = self.wait(peers, lambda states: all(number(item["SESSION"], "owner") == owner and
            number(item["SESSION"], "phase") == 6 and number(item["SESSION"], "media_active") == 1 and
            number(item["RTC"], "connected") == 1 for item in states.values()))
        audio = self.wait(peers, lambda states: all(number(item["AUDIO"], "busy") == 1 and
            number(item["AUDIO"], "device_open") == 1 and number(item["AUDIO"], "owner") == 2 for item in states.values()), audio=True)
        return {"rtc": rtc, "audio": audio}

    def dump(self, peer, name, source=1, require_tone=True, acoustic=False):
        lines = peer.command("AT+AUDIO DUMP", timeout=10)
        header = next((line for line in lines if line.startswith("+PCMA: bytes=")), None)
        assert header and f"bytes=8000 rate=8000 channels=1 source={source}" in header, lines[:5]
        packet = bytearray()
        for line in lines:
            match = re.fullmatch(r"\+PCMA: offset=(\d+) hex=([0-9a-f]{64})", line)
            if match:
                assert int(match[1]) == len(packet), {"expected_offset": len(packet), "line": line}
                packet.extend(bytes.fromhex(match[2]))
        assert len(packet) == 8000, {"bytes": len(packet), "lines": len(lines)}
        path = self.media_directory / f"{name}-{peer.label}.alaw"
        path.write_bytes(packet)
        samples = alaw_decode(packet)
        with wave.open(str(path.with_suffix(".wav")), "wb") as output:
            output.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
            output.writeframes(struct.pack("<8000h", *samples))
        energy = tone_energy(packet, acoustic=acoustic)
        evidence = {"path": str(path), "sha256": hashlib.sha256(packet).hexdigest(), "analysis": energy,
                    "source": source, "proof": "physical microphone recording" if source == 0 else
                    "remote stream PCMA contains digital 400Hz injection; not acoustic speaker or microphone proof"}
        path.with_suffix(".json").write_text(json.dumps(evidence, indent=2), encoding="utf-8")
        if require_tone:
            assert energy["passed"], evidence
        return evidence

    def acoustic_baseline(self):
        self.idle()
        for peer in self.peers:
            peer.command("AT+AUDIO RECORD")
        complete = self.wait(self.peers, lambda states: all(number(item["AUDIO"], "busy") == 0 and
            number(item["AUDIO"], "recorded") == 1 and number(item["AUDIO"], "frames") == 50 and
            number(item["AUDIO"], "stopped") == 0 for item in states.values()), audio=True)
        return {"capture": complete, "baseline": {peer.label: self.dump(peer, "acoustic-baseline", source=0,
            require_tone=False, acoustic=True) for peer in self.peers},
            "measurement_window": {"start_frame": ACOUSTIC_WINDOW_START_FRAME,
                                   "end_frame_exclusive": ACOUSTIC_WINDOW_END_FRAME},
            "proof": "Quiet baseline acquisition only. The same fixed first 200ms settling exclusion applies to baseline and stimulus; does not prove an acoustic path"}

    def acoustic_setup(self):
        if self.args.acoustic_volume is None:
            return {"unchanged": True}
        for peer in self.peers:
            saved = self.wait((peer,), lambda states: controls_saved(states[peer.label]), timeout=10, audio=True)
            state = saved["final"][peer.label]
            self.acoustic_controls[peer.label] = {"volume": number(state["AUDIO"], "volume"),
                                                   "gain": number(state["AUDIO"], "gain")}
            peer.command(f"AT+VOL {self.args.acoustic_volume}")
        applied = self.wait(self.peers, lambda states: all(controls_saved(item,
            {"volume": self.args.acoustic_volume, "gain": self.acoustic_controls[label]["gain"]})
            for label, item in states.items()), timeout=10, audio=True)
        return {"saved_original_controls": self.acoustic_controls.copy(), "applied": applied,
                "test_action": "Explicit temporary speaker volume; microphone gain and DFT criteria unchanged"}

    def acoustic_restore(self):
        evidence = {}
        failures = {}
        fatal = None
        for peer in self.peers:
            if peer.label not in self.acoustic_controls:
                continue
            original = self.acoustic_controls[peer.label]
            try:
                peer.command("AT+AUDIO STOP")
                self.wait((peer,), lambda states: number(states[peer.label]["AUDIO"], "busy") == 0 and
                    number(states[peer.label]["AUDIO"], "device_open") == 0, timeout=10, audio=True)
                peer.command(f"AT+VOL {original['volume']}")
                peer.command(f"AT+MIC {original['gain']}")
                evidence[peer.label] = self.wait((peer,), lambda states: controls_saved(states[peer.label], original),
                                                 timeout=10, audio=True)
                del self.acoustic_controls[peer.label]
            except Exception as error:
                failures[peer.label] = repr(error)
                if isinstance(error, FatalDeviceError):
                    fatal = error
        if fatal:
            raise fatal
        assert not failures, {"restoration_failed": failures, "unrestored_controls": self.acoustic_controls.copy(),
                              "successful_restorations": evidence}
        return evidence

    def acoustic(self, speaker, microphone, baseline):
        self.idle()
        microphone.command("AT+AUDIO RECORD")
        speaker.command("AT+AUDIO TONE")
        complete = self.wait((speaker, microphone), lambda states:
            number(states[microphone.label]["AUDIO"], "busy") == 0 and
            number(states[microphone.label]["AUDIO"], "frames") == 50 and
            number(states[microphone.label]["AUDIO"], "recorded") == 1 and
            number(states[speaker.label]["AUDIO"], "busy") == 0 and
            number(states[speaker.label]["AUDIO"], "frames") == 25, timeout=8, audio=True)
        recorded = self.dump(microphone, f"acoustic-{speaker.label}-to-{microphone.label}", source=0,
                             require_tone=False, acoustic=True)
        comparison = acoustic_comparison(recorded["analysis"], baseline["analysis"])
        evidence = {"speaker": speaker.label, "microphone": microphone.label, "capture": complete,
                    "recording": recorded, "measurement": comparison,
                    "distance_and_ambient_noise_controlled": False}
        (self.media_directory / f"acoustic-{speaker.label}-to-{microphone.label}-path.json").write_text(
            json.dumps(evidence, indent=2), encoding="utf-8")
        assert comparison["passed"], {
            "acoustic_path_unproven": evidence,
            "interpretation": "Board separation and ambient noise are unknown. Missing 400Hz does not by itself diagnose a codec defect."}
        return evidence

    def inject_pair(self):
        self.wait(self.peers, lambda states: all(number(item["STREAM"], "rx") >= 55 for item in states.values()), timeout=10, audio=True)
        before = {peer.label: peer.audio() for peer in self.peers}
        for peer in self.peers:
            assert number(before[peer.label]["STREAM"], "injected") == 0, before
            peer.command("AT+MEDIA TONE")
        finished = self.wait(self.peers, lambda states: all(number(item["STREAM"], "injected") == 25 for item in states.values()), timeout=5, audio=True)
        return {"before": before, "finished": finished}

    def inject_direction(self, source, receiver):
        self.wait((receiver,), lambda states: number(states[receiver.label]["STREAM"], "rx") >= 55,
                  timeout=10, audio=True)
        before = source.audio()
        assert number(before["STREAM"], "injected") == 0, before
        source.command("AT+MEDIA TONE")
        finished = self.wait((source,), lambda states: number(states[source.label]["STREAM"], "injected") == 25,
                             timeout=5, audio=True)
        time.sleep(0.05)
        return {"source": source.label, "receiver": receiver.label, "before": before, "finished": finished}

    def device_case(self, name, caller, callee):
        evidence = {"caller": caller.label, "callee": callee.label}
        if name == "immediate_cancel":
            caller.command("AT+CALL DEVICE " + callee.identity)
            caller.command("AT+CALL HANGUP")
        else:
            evidence["ringing"] = self.dial(caller, callee)
            if name == "reject":
                callee.command("AT+CALL REJECT")
            elif name == "caller_cancel":
                caller.command("AT+CALL HANGUP")
            elif name == "connecting_cancel":
                callee.command("AT+CALL ACCEPT")
                # DEV caller remains OUTGOING while its armed inbound listener
                # waits; the accepting callee owns the outbound connect phase.
                observed = self.wait((callee,), lambda states: number(states[callee.label]["SESSION"], "phase") in (4, 5), timeout=8)
                evidence["observed_connecting"] = observed
                caller.command("AT+CALL HANGUP")
            elif name == "remote_hangup":
                callee.command("AT+CALL ACCEPT")
                evidence["active"] = self.active(self.peers, 2)
                evidence["injection"] = self.inject_pair()
                callee.command("AT+CALL HANGUP")
            else:
                raise ValueError(name)
        evidence["cleanup"] = self.idle()
        if name == "remote_hangup":
            evidence["remote_media"] = {peer.label: self.dump(peer, f"dev-{len(self.stages) + 1}") for peer in self.peers}
        return evidence

    def ai(self, peer):
        start = len(peer.device.lines)
        peer.command("AT+AI START")
        active = self.active((peer,), 1)
        verified = [line for line in peer.device.lines[start:] if "AI profile verified codec=alaw rate=8000 channels=1" in line]
        assert verified, "Missing actual negotiated alaw/8000/1 evidence log"
        generation = active["rtc"]["final"][peer.label]["SESSION"]["generation"]
        before = peer.audio()
        peer.command("AT+AI INTERRUPT")
        peer.command("AT+AI SUBMIT")
        after = self.wait((peer,), lambda states: number(states[peer.label]["STREAM"], "tx") > number(before["STREAM"], "tx") + 5,
                          timeout=15, audio=True)
        same = peer.rtc()
        assert same["SESSION"]["generation"] == generation and number(same["SESSION"], "phase") == 6, same
        peer.command("AT+AI STOP")
        return {"active": active, "negotiated_profile_log": verified, "after_interrupt_submit": after,
                "cleanup": self.idle((peer,))}

    def group(self):
        assert self.args.allow_group_membership_changes, "GROUP requires --allow-group-membership-changes"
        a, b = self.peers
        initial = {peer.label: peer.rtc() for peer in self.peers}
        assert all(number(item["GROUP"], "assigned") == 0 for item in initial.values()), "Existing GROUP membership protected"
        for peer in self.peers:
            peer.command("AT+GROUP REFRESH")
        synced = self.wait(self.peers, lambda states: all(number(item["GROUP"], "known") == 1 and
            number(item["GROUP"], "phase") == 0 and number(item["GROUP"], "assigned") == 0 for item in states.values()))
        a.command("AT+GROUP CREATE")
        created = self.wait((a,), lambda states: number(states[a.label]["GROUP"], "assigned") == 1)
        room = created["final"][a.label]["GROUP"]["room_code"]
        assert re.fullmatch(r"[0-9]{6}", room), created
        self.room = room
        self.room_id = created["final"][a.label]["GROUP"]["room_id"]
        assert self.room_id, created
        b.command("AT+GROUP JOIN " + room)
        joined = self.wait((b,), lambda states: number(states[b.label]["GROUP"], "assigned") == 1 and states[b.label]["GROUP"]["room_code"] == room)
        evidence = {"initial": initial, "synced": synced, "created": created, "joined": joined, "room_code": room, "cycles": []}
        for cycle in range(self.args.group_cycles):
            if cycle:
                for peer in self.peers:
                    peer.command("AT+GROUP CONNECT")
            active = self.active(self.peers, 5)
            self.wait(self.peers, lambda states: all(number(item["GROUP"], "participants") >= 2 for item in states.values()))
            source, receiver = self.peers if cycle % 2 == 0 else tuple(reversed(self.peers))
            source.command("AT+GROUP PTT_DOWN")
            self.wait(self.peers, lambda states: number(states[source.label]["GROUP"], "speaking") == 1 and
                number(states[receiver.label]["GROUP"], "speaking") == 0)
            injection = self.inject_direction(source, receiver)
            source.command("AT+GROUP PTT_UP")
            self.wait(self.peers, lambda states: all(number(item["GROUP"], "speaking") == 0 for item in states.values()))
            for peer in self.peers:
                peer.command("AT+GROUP DISCONNECT")
            cleanup = self.idle()
            retained = {peer.label: peer.rtc() for peer in self.peers}
            assert all(number(item["GROUP"], "assigned") == 1 and item["GROUP"]["room_code"] == room for item in retained.values()), retained
            media = {receiver.label: self.dump(receiver, f"group-{cycle + 1}")}
            evidence["cycles"].append({"active": active, "injection": injection, "cleanup": cleanup, "membership_retained": retained, "media": media})
        evidence["leave"] = self.leave_owned_group()
        return evidence

    def leave_owned_group(self):
        if self.room is None:
            return {"not_created_by_this_run": True}
        evidence = {}
        for peer in self.peers:
            state = peer.rtc()
            group = state["GROUP"]
            if number(group, "assigned"):
                assert group["room_code"] == self.room and group["room_id"] == self.room_id, {"refuse_leave_other_room": state}
                peer.command("AT+GROUP LEAVE")
        evidence["unassigned"] = self.wait(self.peers, lambda states: all(number(item["GROUP"], "assigned") == 0 for item in states.values()))
        evidence["cleanup"] = self.cleanup()
        self.room = self.room_id = None
        return evidence

    def final(self):
        cleanup = self.idle()
        snapshots = {peer.label: peer.snapshot() for peer in self.peers}
        consoles = {peer.label: peer.command("AT+CONSOLE") for peer in self.peers}
        errors = {}
        for peer in self.peers:
            lines = peer.device.lines[peer.verified_line_index:]
            failures = [line for line in lines if re.match(r"E \(\d+\)", line) or any(word in line for word in
                ("Guru Meditation", "assert failed", "CORRUPT HEAP", "Stack protection", "abort()", "rst:0x"))]
            errors[peer.label] = failures
            sof = number(fields(consoles[peer.label])["SOF"], "disconnects")
            assert sof == peer.sof_baseline, {"device": peer.label, "baseline": peer.sof_baseline, "final": sof}
        assert not any(errors.values()), errors
        for snapshot in snapshots.values():
            assert number(snapshot["audio"]["HEAP"], "integrity") == 1
            assert number(snapshot["rtc"]["MEDIA"], "events") == number(snapshot["rtc"]["MEDIA"], "pending") == 0, snapshot
        return {"cleanup": cleanup, "snapshots": snapshots, "consoles": consoles, "errors": errors,
                "heap_note": "Compare idle task/lifecycle state and repeated-cycle trend, not exact TLS/TCP/MQTT heap equality"}


def self_test():
    import contextlib
    import io
    import tempfile
    from types import SimpleNamespace
    from unittest.mock import patch

    assert alaw_decode(bytes((0xd5, 0x55, 0xaa, 0x2a))) == [8, -8, 32256, -32256]
    decoded = alaw_decode(bytes(range(256)))
    encoded = {}
    def encode(sample):
        if sample not in encoded:
            encoded[sample] = min(range(256), key=lambda value: abs(decoded[value] - sample))
        return encoded[sample]
    packet = bytes(encode(round(3000 * math.sin(2 * math.pi * 400 * i / 8000))) for i in range(8000))
    assert tone_energy(packet)["passed"]
    assert not tone_energy(bytes([0xd5]) * 8000)["passed"]
    wrong_frequency = bytes(encode(round(3000 * math.sin(2 * math.pi * 350 * i / 8000))) for i in range(8000))
    assert not tone_energy(wrong_frequency)["passed"]
    too_quiet = bytes(encode(round(50 * math.sin(2 * math.pi * 400 * i / 8000))) for i in range(8000))
    assert not tone_energy(too_quiet)["passed"]
    short_tone = packet[:7 * 160] + bytes([0xd5]) * (8000 - 7 * 160)
    assert not tone_energy(short_tone)["passed"]
    silence = bytes([0xd5]) * 8000
    startup = bytes(encode(round(25000 * math.sin(2 * math.pi * 50 * i / 8000) +
                                 3000 * math.sin(2 * math.pi * 400 * i / 8000))) for i in range(5 * 160))
    baseline = tone_energy(startup + silence[len(startup):], acoustic=True)
    stimulus = tone_energy(startup + silence[5 * 160:12 * 160] + packet[:25 * 160] + silence[37 * 160:], acoustic=True)
    assert acoustic_comparison(stimulus, baseline)["passed"]
    ambient = tone_energy(startup + packet[len(startup):], acoustic=True)
    assert not acoustic_comparison(ambient, ambient)["passed"]
    assert not acoustic_comparison(tone_energy(wrong_frequency, acoustic=True), baseline)["passed"]
    assert not acoustic_comparison(tone_energy(silence[:12 * 160] + packet[:7 * 160] + silence[19 * 160:], acoustic=True), baseline)["passed"]
    assert acoustic_comparison(stimulus, baseline)["window"]["start_ms"] == 200

    original = {"volume": 57, "gain": 11}
    saved = {"AUDIO": {"busy": "0", "device_open": "0", "volume": "57", "gain": "11"},
             "SETTINGS": {"loaded": "1", "saved": "1", "pending": "0", "error": "0"}}
    assert controls_saved(saved, original)
    for key, value in (("loaded", "0"), ("saved", "0"), ("pending", "1"), ("error", "-1")):
        invalid = {**saved, "SETTINGS": {**saved["SETTINGS"], key: value}}
        assert not controls_saved(invalid, original)
    controls = object.__new__(Regression)
    controls.acoustic_controls = {"A": original.copy(), "B": original.copy()}
    controls.peers = [SimpleNamespace(label=label, command=lambda command: ["OK"]) for label in ("A", "B")]
    def restoration_wait(peers, predicate, **kwargs):
        if peers[0].label == "A":
            raise TimeoutError("Values applied but settings not saved")
        assert predicate({"B": saved})
        return {"final": {"B": saved}}
    controls.wait = restoration_wait
    try:
        controls.acoustic_restore()
    except AssertionError:
        pass
    else:
        raise AssertionError("Unsaved restoration must fail")
    assert controls.acoustic_controls == {"A": original}
    controls.wait = lambda peers, predicate, **kwargs: {"final": {"A": saved}} if predicate({"A": saved}) else None
    controls.acoustic_restore()
    assert not controls.acoustic_controls

    retirement = {"SESSION": {"owner": "0", "phase": "8", "media_active": "0"},
                  "RTC": {"initialized": "1", "online": "1", "connecting": "0", "connected": "0",
                          "closing": "0", "stopping": "0", "sdk_pending": "0"},
                  "MEDIA": {"events": "0", "pending": "0"}}
    assert retired(retirement)
    for key in ("connecting", "connected", "closing", "stopping", "sdk_pending"):
        assert not retired({**retirement, "RTC": {**retirement["RTC"], key: "1"}})
    assert not retired({**retirement, "MEDIA": {"events": "0", "pending": "1"}})

    class RetiringPeer:
        label, verified, fatal = "A", True, None
        def __init__(self):
            self.reads, self.commands, self.dismissed = 0, [], False
        def check_fatal(self, raise_error=True):
            pass
        def rtc(self):
            self.reads += 1
            self.last = {**retirement, "SESSION": {**retirement["SESSION"], "phase": "0" if self.dismissed else "8"},
                         "RTC": {**retirement["RTC"], "sdk_pending": "1" if self.reads < 3 else "0"}}
            return self.last
        def audio(self):
            return saved
        def command(self, text):
            assert text == "AT+CALL DISMISS" and self.reads >= 3 and retired(self.last)
            self.commands.append(text)
            self.dismissed = True
            return ["OK"]
    retirement_test = object.__new__(Regression)
    retiring = RetiringPeer()
    retirement_test.peers = [retiring]
    retirement_test.cleanup()
    assert retiring.commands == ["AT+CALL DISMISS"]

    class FaultDevice:
        def __init__(self):
            self.lines, self.sent, self.commands = [], [], 0
        def send(self, data):
            self.sent.append(data)
        def response(self, start, timeout):
            if len(self.sent) == 1:
                self.lines.extend(("Guru Meditation Error: Store access fault", "rst:0xc (RTC_SW_CPU_RST)"))
                raise TimeoutError("No terminal AT response")
            self.lines.extend(("+MEMTRACE: live=15220 failures=0", "OK"))
            return self.lines[start:]
    fault = object.__new__(Peer)
    fault.label, fault.verified, fault.verified_line_index, fault.fatal = "B", True, 0, None
    fault.device = FaultDevice()
    for command in ("AT+CALL ACCEPT", "AT+CALL DISMISS"):
        try:
            fault.command(command)
        except FatalDeviceError:
            pass
        else:
            raise AssertionError("Panic must stop mutating commands, including dismiss after reset")
    assert len(fault.device.sent) == 1 and fault.fatal["capture_restarted"]
    assert "AT+MEMTRACE" in fault.diagnostic()
    retirement_test.peers = [fault]
    writes_before_cleanup = len(fault.device.sent)
    fatal_cleanup_result = retirement_test.cleanup()
    assert fatal_cleanup_result["fatal_targets_not_mutated"] == ["B"]
    assert fatal_cleanup_result["fatal_targets_retirement_proven"] is False
    assert len(fault.device.sent) == writes_before_cleanup
    assert fields(["+SESSION: owner=2 phase=6 peer=abc room= reason="])["SESSION"]["phase"] == "6"

    class ScriptedPeer:
        label = "A"
        def __init__(self, phases):
            self.phases = iter(phases)
            self.reads = 0
        def rtc(self):
            self.reads += 1
            return {"SESSION": {"phase": str(next(self.phases))}}

    regression = object.__new__(Regression)
    peer = ScriptedPeer((2, 4, 6))
    verified = regression.wait((peer,), lambda states: number(states["A"]["SESSION"], "phase") == 6, timeout=1)
    assert peer.reads == 3 and number(verified["final"]["A"]["SESSION"], "phase") == 6
    try:
        regression.wait((ScriptedPeer((8,)),), lambda states: False, timeout=1)
    except AssertionError as error:
        assert "terminal_business_error" in str(error)
    else:
        raise AssertionError("Terminal state must not be counted as an accepted-command success")

    order, constructed = [], []
    class MainPeer:
        def __init__(self, label, *args):
            self.label, self.verified, self.fatal = label, True, None
            self.port_closed = self.log_closed = False
            self.device = SimpleNamespace(port=SimpleNamespace(close=self.close_port), log=SimpleNamespace(close=self.close_log))
            constructed.append(self)
        def close_port(self):
            self.port_closed = True
        def close_log(self):
            self.log_closed = True
        def diagnostic(self):
            order.append("diagnostic-" + self.label)
            return {"AT+MEMTRACE": ["+MEMTRACE: failures=1"], "fatal": self.fatal}
    def panic_case(regression, *args):
        regression.peers[1].fatal = {"markers": [{"line": "Guru Meditation Error"}], "capture_restarted": True}
        raise FatalDeviceError("B", regression.peers[1].fatal)
    def fatal_cleanup(regression):
        order.append("cleanup")
        return {"reset_is_not_retirement": True}
    with tempfile.TemporaryDirectory() as directory:
        output = Path(directory) / "PRIVATE" / "product-host-test"
        argv = ["regress_product.py", "--ports", "COM8", "COM9", "--macs", "14:63:93:e1:ca:88", "14:63:93:e1:d8:f8",
                "--device-ids", "A", "B", "--elf-sha", "a" * 64, "--output", str(output), "--skip-group"]
        output_log = io.StringIO()
        with contextlib.redirect_stdout(output_log), patch("sys.argv", argv), patch(__name__ + ".Peer", MainPeer), \
             patch.object(Regression, "ready", return_value={}), patch.object(Regression, "device_case", autospec=True, side_effect=panic_case) as cases, \
             patch.object(Regression, "cleanup", autospec=True, side_effect=fatal_cleanup), patch.object(Regression, "ai") as ai:
            assert main() == 1 and cases.call_count == 1 and ai.call_count == 0
        assert "dev-immediate_cancel: FAIL" in output_log.getvalue()
        result = json.loads((output / "result.json").read_text(encoding="utf-8"))
        failure = json.loads((output / "stages" / "02-dev-immediate_cancel.json").read_text(encoding="utf-8"))
        assert not result["selected_scope_passed"] and result["fatal_devices"]["B"]["capture_restarted"]
        assert "AT+MEMTRACE" in failure["diagnostic"]["B"]
        assert order.index("diagnostic-B") < order.index("cleanup")
        assert all(peer.port_closed and peer.log_closed for peer in constructed)
    print("product host self-test passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ports", nargs=2)
    parser.add_argument("--macs", nargs=2)
    parser.add_argument("--device-ids", nargs=2)
    parser.add_argument("--elf-sha")
    parser.add_argument("--output", type=Path)
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--group-cycles", type=int, default=2)
    parser.add_argument("--allow-group-membership-changes", action="store_true")
    parser.add_argument("--skip-group", action="store_true")
    parser.add_argument("--acoustic", action="store_true", help="Physical cross-board speaker-to-microphone tone test, separately from network PCMA")
    parser.add_argument("--only-acoustic", action="store_true", help="Readiness, acoustic fixtures and final cleanup only; no cloud call/GROUP actions")
    parser.add_argument("--acoustic-volume", type=int, help="Explicit temporary acoustic-test volume, restored with the original gain in finally")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.only_acoustic:
        args.acoustic = True
    if args.self_test:
        self_test()
        return 0
    if not all((args.ports, args.macs, args.device_ids, args.elf_sha, args.output)):
        parser.error("--ports/--macs/--device-ids/--elf-sha/--output are required")
    if len(set(args.ports)) != 2 or len(set(args.macs)) != 2 or len(set(args.device_ids)) != 2:
        parser.error("Two distinct explicit targets are required")
    if not re.fullmatch(r"[0-9a-fA-F]{64}", args.elf_sha) or not all(re.fullmatch(r"[0-9a-fA-F]{2}(:[0-9a-fA-F]{2}){5}", mac) for mac in args.macs):
        parser.error("Invalid MAC or full ELF SHA-256")
    if not all(re.fullmatch(r"[A-Za-z0-9_-]{1,64}", identity) for identity in args.device_ids):
        parser.error("Device IDs must be bounded ASCII identities, without AT delimiters")
    if not 1 <= args.cycles <= 100 or not 2 <= args.group_cycles <= 20:
        parser.error("cycles must be 1..100; group-cycles 2..20 for both directions")
    if args.acoustic_volume is not None and (not args.acoustic or not 0 <= args.acoustic_volume <= 100):
        parser.error("--acoustic-volume requires --acoustic and a value in 0..100")
    if "PRIVATE" not in args.output.resolve().parts:
        parser.error("Evidence output must be in a PRIVATE directory")
    args.output.mkdir(parents=True, exist_ok=False)
    peers = [Peer(label, port, mac, identity, args.elf_sha, args.output / label)
             for label, port, mac, identity in zip(("A", "B"), args.ports, args.macs, args.device_ids)]
    regression = Regression(peers, args)
    try:
        if not regression.stage("readiness", regression.ready):
            return 1
        if args.acoustic:
            baseline_result = {}
            def acquire_baseline():
                baseline_result.update(regression.acoustic_baseline())
                return baseline_result
            if regression.stage("acoustic-setup", regression.acoustic_setup) and regression.stage("acoustic-baseline", acquire_baseline):
                for speaker, microphone in (peers, tuple(reversed(peers))):
                    regression.stage(f"acoustic-{speaker.label}-to-{microphone.label}",
                        lambda speaker=speaker, microphone=microphone: regression.acoustic(speaker, microphone,
                            baseline_result["baseline"][microphone.label]))
            if not regression.stage("acoustic-restore", regression.acoustic_restore):
                return 1
            if not regression.stage("cleanup-acoustic", regression.cleanup):
                return 1
            if args.only_acoustic:
                regression.stage("final", regression.final)
                return 0 if all(stage["passed"] for stage in regression.stages) else 1
        for case in ("immediate_cancel", "caller_cancel", "reject", "connecting_cancel"):
            regression.stage("dev-" + case, lambda case=case: regression.device_case(case, peers[0], peers[1]))
            if not regression.stage("cleanup-" + case, regression.cleanup):
                return 1
        for cycle in range(args.cycles):
            caller, callee = peers if cycle % 2 == 0 else tuple(reversed(peers))
            regression.stage(f"dev-active-{cycle + 1}", lambda: regression.device_case("remote_hangup", caller, callee))
            if not regression.stage(f"cleanup-active-{cycle + 1}", regression.cleanup):
                return 1
        for peer in peers:
            regression.stage("ai-" + peer.label, lambda peer=peer: regression.ai(peer))
            if not regression.stage("cleanup-ai-" + peer.label, regression.cleanup):
                return 1
        if not args.skip_group:
            regression.stage("group", regression.group)
            regression.stage("cleanup-group-membership", regression.leave_owned_group)
            regression.stage("cleanup-group-session", regression.cleanup)
        regression.stage("final", regression.final)
    except FatalDeviceError:
        regression.stage("fatal-cleanup", regression.cleanup, propagate_fatal=False)
    finally:
        if regression.acoustic_controls:
            regression.stage("emergency-acoustic-restore", regression.acoustic_restore, propagate_fatal=False)
        if regression.room is not None and all(peer.verified for peer in peers):
            regression.stage("emergency-owned-group-cleanup", regression.leave_owned_group, propagate_fatal=False)
        for peer in peers:
            peer.device.port.close()
            peer.device.log.close()
        result = {"selected_scope_passed": bool(regression.stages) and all(stage["passed"] for stage in regression.stages),
                  "stages": [{key: item[key] for key in ("name", "passed", "seconds")} for item in regression.stages],
                  "targets": [{"port": port, "mac": mac, "device_id": identity} for port, mac, identity in zip(args.ports, args.macs, args.device_ids)],
                  "full_product_acceptance": False,
                  "not_proven": ["speech intelligibility and echo", "physical keys", "WeChat phone call/authorization", "weak-network and long-run"] +
                  ([] if args.acoustic and all(any(stage["name"] == name and stage["passed"] for stage in regression.stages)
                    for name in ("acoustic-A-to-B", "acoustic-B-to-A")) else ["physical speaker/microphone acoustics"]),
                  "only_acoustic_scope": args.only_acoustic,
                  "group_skipped": args.skip_group, "owned_test_room_not_removed": regression.room,
                  "fatal_devices": {peer.label: peer.fatal for peer in peers if peer.fatal},
                  "unrestored_controls": regression.acoustic_controls.copy()}
        (args.output / "result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
        print(json.dumps(result), flush=True)
    return 0 if result["selected_scope_passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
