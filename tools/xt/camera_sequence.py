"""Scoped camera diagnostics; no flash, configuration, or remote UI actions."""

import time

from camera_evidence import probe_frame, probe_snapshot, video_snapshot
from console_sdk import ConsoleError, fields
from gate_ai import STATUS as AI_STATUS, UINT32_MAX, snapshot as ai_snapshot
from sdk_bench import one, state, unsigned


PROBE = "AT+VIDEO PROBE"
AI_START, AI_STOP = "AT+AI START", "AT+AI STOP"
VISION = "AT+STATUS ai_vision"
STEPS = {"probe": (PROBE,), "ai-vision": (AI_START, AI_STOP), "h5": ()}
H5_UNVERIFIED = "no verified remote start, render witness and session-specific stop adapter"
AI_LIVE = {"listening", "thinking", "speaking"}
AI_STARTING = {"connecting"}
AI_ENDING = {"ending"}
AI_READS = (AI_STATUS, VISION, "AT+NET", "AT+PLATFORM", "AT+LINK")


def plan(step, cycle, index):
    return {"cycle": cycle, "index": index, "kind": step, "verdict": "unverified",
            "first_frame": None, "first_observed_ms": None, "first_luma": None, "recovery": None,
            "cleanup_verified": None}


def summarize(steps, rounds, run_step=None):
    """Run only the requested steps and preserve every considered round."""
    if not steps or any(step not in STEPS for step in steps) or not 1 <= rounds <= 10 or len(steps) * rounds > 60:
        raise ConsoleError("camera sequence requires known steps and at most 60 actions over 1..10 rounds")
    coverage = {step: "unverified" for step in STEPS}
    rows = []
    failed = False
    for cycle in range(1, rounds + 1):
        for index, step in enumerate(steps, 1):
            row = plan(step, cycle, index)
            if step == "h5":
                row["reason"] = H5_UNVERIFIED
            elif failed or run_step is None:
                row["reason"] = "not run after failure" if failed else "serial diagnostic unavailable"
            else:
                try:
                    run_step(row)
                except (ConsoleError, ValueError, OSError, TimeoutError) as error:
                    row.update(verdict="fail", reason=str(error))
                    failed = True
            rows.append(row)
    for step in STEPS:
        outcomes = {row["verdict"] for row in rows if row["kind"] == step}
        if "fail" in outcomes:
            coverage[step] = "fail"
        elif outcomes == {"pass"}:
            coverage[step] = "pass"
    result = "fail" if failed else "pass" if all(row["verdict"] == "pass" for row in rows) else "unverified"
    return {"test": "camera-sequence", "verdict": result, "coverage": coverage, "rounds": rows}


def video_idle(video):
    return (video["requested"] == video["generation"] == 0 and
            video["up_phase"] == video["down_phase"] == "off")


class Sequence:
    """Use gate's already-selected exclusive port, SDK Session and identity witness."""

    def __init__(self, port, session, witness, confirm=None, timeout=15, *, clock=time.monotonic, sleep=time.sleep):
        if not 1 <= timeout <= 60:
            raise ConsoleError("camera wait must be in 1..60 seconds")
        self.port, self.session, self.witness, self.confirm = port, session, witness, confirm
        self.timeout, self.clock, self.sleep = timeout, clock, sleep

    def confirm_device(self):
        try:
            if self.confirm:
                self.confirm()
        except (ConsoleError, ValueError, OSError):
            self.session.usable = False
            raise

    def checkpoint(self, idle=False):
        self.confirm_device()
        return self.witness.checkpoint(idle=idle)

    def read_video(self):
        return video_snapshot(self.session.send("AT+VIDEO"))

    def require_commands(self, reads, actions):
        if any(not self.session.has(command) for command in (*reads, *actions)):
            return False
        if (any(self.session.check(command).access != "read" for command in reads) or
                any(self.session.check(command).access != "action" for command in actions)):
            raise ConsoleError("camera sequence refuses a different AT access classification")
        return True

    def wait(self, reader, accept, message):
        # Keep the specific reason for an invalid sample instead of converting
        # every problem in the poller into a timeout.
        end = self.clock() + self.timeout
        while self.clock() < end:
            value = reader()
            if accept(value):
                return value
            self.sleep(min(0.15, max(0, end - self.clock())))
        raise ConsoleError(message)

    def __call__(self, row):
        kind = row["kind"]
        if kind == "h5":
            row["reason"] = H5_UNVERIFIED
            return
        actions = STEPS[kind]
        if not set(actions).issubset(self.session.permits):
            row["reason"] = "exact local camera actions are not authorized"
            return
        reads = ("AT+VERSION", "AT+STATE", "AT+VIDEO", *(AI_READS if kind == "ai-vision" else ()))
        if not self.require_commands(reads, actions):
            row["reason"] = "selected firmware does not expose the required diagnostic or action"
            return
        row["before"] = self.checkpoint(idle=True)
        if not video_idle(self.read_video()):
            raise ConsoleError("camera sequence requires video to be idle before an action")
        if kind == "probe":
            self.probe(row)
        else:
            self.ai(row)

    def run_once(self, kind):
        return summarize([kind], 1, self)["rounds"][0]

    def probe(self, row):
        self.confirm_device()
        started = self.clock()
        reply = self.session.send(PROBE)
        boundary = self.port.sdk_start + len(reply)

        def sample():
            # The console sends notifications after the action terminator.
            # Reuse the exchange's drained boundary, not a mark before write.
            lines = self.port.since(boundary)
            if row["first_frame"] is None:
                frame = probe_frame(lines)
                if frame is not None:
                    row["first_frame"] = frame
                    row["first_observed_ms"] = round((self.clock() - started) * 1000)
            return probe_snapshot(lines)

        report = self.wait(sample, bool, "camera probe did not produce a fresh terminal notification")
        row.update(first_luma=report["first_luma"], probe_frames=report["frames"], probe_error=report["error"])
        if (report["result"] != "passed" or report["frames"] != 3 or not report["first_frame"] or
                not report["first_frame"]["ok"]):
            raise ConsoleError("camera probe did not confirm three frames and native cleanup")
        if not video_idle(self.read_video()):
            raise ConsoleError("video was no longer idle after the probe")
        row["after"] = self.checkpoint(idle=True)
        # Catch duplicate or contradictory records already delivered during
        # the closing queries. No further probe is issued on ambiguity.
        if probe_snapshot(self.port.since(boundary)) != report:
            raise ConsoleError("probe observations changed after its terminal notification")
        row.update(verdict="pass", cleanup_verified=True)

    def ai_vision(self):
        return fields(self.session.send(VISION), "+AI_VISION: ")

    def ai_preflight(self):
        if ai_snapshot(self.session)[0] != "idle":
            raise ConsoleError("AI must be idle before camera sequence start")
        try:
            vision = self.ai_vision()
        except ConsoleError as error:
            if str(error).endswith(": device answered NOT_FOUND SECTION") and self.session.usable:
                return "selected firmware did not register AI vision"
            raise
        if unsigned(vision, "state") == 4 or vision.get("enabled") == "0":
            return "AI vision is disabled or unsupported; settings left unchanged"
        if vision.get("enabled") != "1" or unsigned(vision, "state") != 0 or unsigned(vision, "link") != 0:
            raise ConsoleError("AI vision did not report a usable idle state")
        if (one(self.session, "AT+NET", "+NET: ").get("connected") != "1" or
                one(self.session, "AT+PLATFORM", "+PLATFORM: ").get("online") != "1"):
            return "AI vision requires the existing connected and online device"
        link = one(self.session, "AT+LINK", "+LINK: ")
        if link.get("started") != "1":
            return "AI vision requires the existing RTC service to be online"
        return None

    def ai(self, row):
        reason = self.ai_preflight()
        if reason:
            row["reason"] = reason
            return
        owned_gen = owned_link = 0
        lost_ownership = False
        self.checkpoint(idle=True)
        self.confirm_device()
        phase, _, _, _, baseline, _ = ai_snapshot(self.session)
        if phase != "idle":
            raise ConsoleError("AI must still be idle immediately before camera sequence start")
        expected_started = (baseline + 1) & UINT32_MAX
        started = self.clock()
        try:
            self.session.send(AI_START)
        except (ConsoleError, OSError) as error:
            if isinstance(error, OSError):
                self.session.usable = False
            if not self.session.usable:
                row.update(cleanup_verified=False, cleanup_failure="start outcome unknown; no stop sent")
            raise

        def observe_owner():
            nonlocal lost_ownership
            try:
                return ai_snapshot(self.session)
            except (ConsoleError, OSError):
                lost_ownership = True
                raise

        def sample():
            nonlocal owned_gen, owned_link, lost_ownership
            phase, origin, generation, link, count, _ = observe_owner()
            if phase == "idle" and count == baseline and not owned_gen:
                return False
            if (count != expected_started or not generation or origin != "user" or
                    (owned_gen and generation != owned_gen)):
                lost_ownership = True
                raise ConsoleError("AI camera sequence lost the owned activity generation")
            owned_gen = generation
            row["activity_generation"] = generation
            if owned_link and link != owned_link and phase not in AI_ENDING:
                lost_ownership = True
                raise ConsoleError("AI camera sequence lost the owned video link generation")
            if phase in AI_ENDING:
                raise ConsoleError("AI activity was ending before a video frame was observed")
            if not link or phase not in AI_LIVE:
                return False
            owned_link = link
            row["link_generation"] = link
            vision = self.ai_vision()
            vstate, vlink = unsigned(vision, "state"), unsigned(vision, "link")
            video = self.read_video()
            current = observe_owner()
            if current[1:5] != ("user", generation, link, expected_started):
                lost_ownership = True
                raise ConsoleError("AI activity changed while taking a camera sample")
            if current[0] not in AI_LIVE:
                raise ConsoleError("AI activity changed while taking a camera sample")
            if vstate == 3 and vlink == link:
                row["vision_error"] = unsigned(vision, "error")
            if video["requested"] == video["generation"] == link:
                row.update(recovery=video["recovery"], video=video)
                if row["first_frame"] is None or row["first_luma"] is None:
                    row["first_luma"] = video["first_luma"]
                if video["captured"] > 0 and row["first_frame"] is None:
                    row["first_frame"] = {"captured": video["captured"], "sent": video["sent"]}
                    row["first_observed_ms"] = round((self.clock() - started) * 1000)
                failed = any(video[key] for key in ("capture_failed", "encode_failed", "send_failed"))
                if video["up_phase"] == "error" or video["error"] not in {"0x0", "NONE", "OK"} or failed:
                    raise ConsoleError("AI vision reported a video error in the owned generation")
                if (vstate == 2 and vlink == link and video["up_phase"] == "running" and
                        video["sent"] > 0 and row["first_frame"] is not None):
                    return True
            if vstate == 3 and vlink == link:
                raise ConsoleError("AI vision failed in the owned generation")
            return False

        try:
            self.wait(sample, bool, "AI vision did not send a frame in the owned generation")
        finally:
            self.finish_ai(row, owned_gen, owned_link, expected_started, lost_ownership)
        if not row["cleanup_verified"]:
            raise ConsoleError("AI camera sequence could not confirm release")
        row["verdict"] = "pass"

    def finish_ai(self, row, owned_gen, owned_link, expected_started, lost_ownership):
        row["cleanup_verified"] = False
        try:
            if not self.session.usable:
                raise ConsoleError("serial outcome or running identity could not be reverified")
            if lost_ownership or not owned_gen:
                raise ConsoleError("no provably owned AI generation to stop; left the device untouched")
            self.checkpoint()
            self.confirm_device()

            def current_ai():
                phase, origin, generation, link, count, _ = ai_snapshot(self.session)
                if (count != expected_started or (phase != "idle" and
                        (generation != owned_gen or origin != "user" or
                         (owned_link and link != owned_link and phase not in AI_ENDING)))):
                    raise ConsoleError("no provably owned AI generation to stop; left the device untouched")
                return phase

            phase = current_ai()
            if phase != "idle":
                if phase in AI_LIVE | AI_STARTING:
                    self.session.send(AI_STOP)
                elif phase not in AI_ENDING:
                    raise ConsoleError("AI activity is in an unknown phase; no stop sent")

            def released():
                return (current_ai() == "idle" and
                        state(self.session).get("activity") == "none" and video_idle(self.read_video()))

            self.wait(released, bool, "AI activity or camera did not return to idle")
            row["after"] = self.checkpoint(idle=True)
            if current_ai() != "idle":
                raise ConsoleError("AI did not remain idle after camera sequence cleanup")
            row["cleanup_verified"] = True
        except (ConsoleError, ValueError, OSError, TimeoutError) as error:
            row["cleanup_failure"] = str(error)
