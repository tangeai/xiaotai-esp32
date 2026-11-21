"""Run the SDK bring-up gate with device-bound, exact action permissions."""

from contextlib import closing, contextmanager
from dataclasses import dataclass
import json
from pathlib import Path
import re
from types import SimpleNamespace

import app_stacks
import boot_check
import sdk_bench
from console_sdk import ConsoleError, Session, command_line, load_vocabulary
from sdk_soak import Evidence


ITEMS = (
    ("boot", "开机"), ("network", "联网"), ("serial", "串口"), ("screen", "屏幕或无屏交互"),
    ("ai", "AI 对讲"), ("device_call", "设备互呼"), ("wechat", "微信语音"), ("room", "房间"),
    ("camera", "相机"), ("memory_stack", "内存和栈"),
)
ACTIVE_ITEMS = {"boot", "ai", "device_call", "wechat", "room", "camera"}
PRODUCT = re.compile(r"[a-z0-9][a-z0-9-]{0,79}")
SERIAL = re.compile(r"[A-Za-z0-9_.:-]{1,128}")
SHA = re.compile(r"[0-9a-fA-F]{64}")
PHONE_ANSWER = "PHONE_WX_OUT_ANSWER"


class Unverified(ConsoleError):
    """A prerequisite was absent; the requested operation was not run."""


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ConsoleError("duplicate key in gate allow-file")
        result[key] = value
    return result


def object_keys(obj, required, optional=()):
    if not isinstance(obj, dict) or not set(required) <= set(obj) <= set(required) | set(optional):
        raise ConsoleError("invalid gate allow-file fields")


def word(value, pattern, description):
    if not isinstance(value, str) or not pattern.fullmatch(value):
        raise ConsoleError(f"invalid gate {description}")
    return value


def actions(value, parser):
    if not isinstance(value, list) or len(value) > 64:
        raise ConsoleError("gate actions must be an explicit bounded list")
    result = [parser(item) for item in value]
    if len(set(result)) != len(result):
        raise ConsoleError("duplicate gate action")
    return frozenset(result)


@dataclass(frozen=True)
class Target:
    name: str
    port: str
    product: str
    usb: str
    sha: str
    words: Path
    vocab: object
    chip: str
    app: str
    permits: frozenset
    parts: dict
    info: dict


def load_target(config, selector, product, expected, base, root, resolve):
    object_keys(config, ("usb_serial", "elf_sha256", "commands", "actions"), ("app", "product"))
    if config.get("product", product) != product:
        raise ConsoleError("allow-file product does not match the selected product")
    sha = word(config["elf_sha256"], SHA, "ELF SHA-256").lower()
    if expected and sha != word(expected, SHA, "ELF SHA-256").lower():
        raise ConsoleError("allow-file ELF differs from the selected build")
    usb = word(config["usb_serial"], SERIAL, "USB descriptor serial")
    if not isinstance(config["commands"], str) or not config["commands"]:
        raise ConsoleError("gate requires the commands.json of the selected build")
    words = (base / config["commands"]).resolve()
    vocab = load_vocabulary(words)
    if vocab.product != product or not vocab.board:
        raise ConsoleError("gate vocabulary does not match the selected product")
    profile = json.loads((root / "products" / product / "product.json").read_text(encoding="utf-8"))
    board = json.loads((root / "boards" / profile["board"] / "board.json").read_text(encoding="utf-8"))
    apps = profile.get("applications", [])
    app = config.get("app", profile.get("home") or (apps[0] if len(apps) == 1 else None))
    if not isinstance(app, str) or app not in apps:
        raise ConsoleError("gate needs an expected foreground app from the product")
    name, info = resolve(selector)
    permits = actions(config["actions"], lambda value: value if value == boot_check.ACTION else command_line(value))
    return Target(name, info["port"], product, usb, sha, words, vocab, board["target"], app, permits, board["parts"], info)


def load(path, root, args, resolve):
    if not path.is_file() or path.stat().st_size > 65536:
        raise ConsoleError("gate requires a regular allow-file of at most 64 KiB")
    data = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=unique)
    object_keys(data, ("schema", "primary"), ("peer", "phone", "stack_tasks", "isolated_bench"))
    if type(data["schema"]) is not int or data["schema"] != 1:
        raise ConsoleError("unsupported gate allow-file schema")
    isolated = data.get("isolated_bench", False)
    if type(isolated) is not bool:
        raise ConsoleError("isolated_bench must be a JSON boolean")
    primary = load_target(data["primary"], args.dev, word(args.product, PRODUCT, "product"),
                          args.elf_sha256, path.parent, root, resolve)
    peer = None
    if args.peer:
        if not isinstance(data.get("peer"), dict):
            raise ConsoleError("--peer requires an independent, device-bound peer grant")
        peer = load_target(data["peer"], args.peer, word(data["peer"].get("product"), PRODUCT, "peer product"),
                           None, path.parent, root, resolve)
        if primary.port.casefold() == peer.port.casefold() or primary.usb.casefold() == peer.usb.casefold():
            raise ConsoleError("gate peer must be a different USB device and serial port")
    elif "peer" in data:
        raise ConsoleError("peer grant in allow-file requires explicit --peer")
    phone = None
    if args.phone:
        config = data.get("phone")
        object_keys(config, ("serial", "actions"))
        phone = SimpleNamespace(serial=word(config["serial"], SERIAL, "ADB serial"),
                                permits=actions(config["actions"], lambda value: word(value,
                                    re.compile(r"PHONE_WX_(?:IN|OUT_ANSWER)"), "phone action")))
    elif "phone" in data:
        raise ConsoleError("phone grant in allow-file requires explicit --phone")
    tasks = data.get("stack_tasks", [])
    if not isinstance(tasks, list) or not all(isinstance(name, str) for name in tasks):
        raise ConsoleError("stack_tasks must list the expected task names")
    app_stacks.validate_request(primary.sha, tasks, 1024)
    return primary, peer, phone, tasks, isolated


def result(status, detail, facts=None):
    return {"status": status, "detail": detail, "facts": facts or {}}


class Gate:
    def __init__(self, kit, primary, peer, phone, tasks, isolated, out):
        self.kit, self.primary, self.peer, self.phone, self.tasks, self.out = kit, primary, peer, phone, tasks, out
        self.isolated = isolated

    def confirm(self, target):
        ports = self.kit.list_ports.comports()
        found = [port for port in ports if (port.serial_number or "").casefold() == target.usb.casefold()]
        if len(found) != 1 or found[0].device.casefold() != target.port.casefold():
            raise Unverified("USB identity is missing, ambiguous or on a different port")
        return found[0].device

    @contextmanager
    def open(self, target):
        try:
            port = self.kit.Port(self.confirm(target), exclusive=True)
        except OSError as error:
            raise Unverified("serial transport is unavailable") from error
        try:
            try:
                session = Session(port.sdk_exchange, target.vocab, target.permits - {boot_check.ACTION}, target.sha)
            except ConsoleError as error:
                raise Unverified(str(error)) from error
            if session.device["chip"] != target.chip:
                raise Unverified("running chip does not match the selected build")
            witness = Evidence(session, port, port.mark(), self.kit.BOOT_BANNER)
            yield SimpleNamespace(port=port, session=session, witness=witness, confirm=lambda: self.confirm(target))
        finally:
            port.close()

    def ready(self, endpoint, target):
        # Do not start a session while the selected app is changing or offline.
        def check():
            row = sdk_bench.state(endpoint.session)
            return ((row.get("app"), row.get("activity"), row.get("id")) == (target.app, "none", "0")
                    and boot_check.network(endpoint.session) and boot_check.platform(endpoint.session))

        try:
            sdk_bench.poll(check, bool, 12)
        except ConsoleError as error:
            raise Unverified("app, network or platform was not ready for a test session") from error
        endpoint.confirm()

    def boot(self):
        target = self.primary
        if boot_check.ACTION not in target.permits:
            return result("not_verified", "USB chip reset was not authorized")
        if target.chip not in boot_check.CHIPS:
            return result("not_verified", "automatic reset is not supported on this chip/transport")
        try:
            from esptool.reset import HardReset
        except ImportError as error:
            raise Unverified("esptool native USB reset support unavailable") from error
        if not callable(getattr(HardReset, "reset", None)):
            raise Unverified("esptool native USB reset support unavailable")

        def bound_usb():
            try:
                return boot_check.bind_usb(self.kit.list_ports.comports(), target.port, target.usb)
            except ConsoleError as error:
                raise Unverified("automatic reset requires the confirmed native USB Serial/JTAG port") from error

        bound_usb()
        report = boot_check.run(lambda: closing(self.kit.Port(bound_usb(), exclusive=True)),
                                lambda port: boot_check.chip_reset(port, HardReset, bound_usb), bound_usb,
                                target.vocab, target.sha, target.app, 3, 45)
        if report["pass"]:
            return result("pass", "three native USB chip resets reached the expected app", report)
        verified_attempt = any(row["rom_reset"] and row["boot"] for row in report["rounds"])
        return result("fail" if verified_attempt else "not_verified", "three verified chip boots did not complete", report)

    def network(self, endpoint):
        session = endpoint.session
        reads = ("AT+STATE", "AT+NET", "AT+PLATFORM")
        if any(not session.has(command) or session.check(command).access != "read" for command in reads):
            raise Unverified("the selected firmware did not advertise all read-only network diagnostics")
        try:
            self.ready(endpoint, self.primary)
        except Unverified:
            net, online = boot_check.network(session), boot_check.platform(session)
            return result("fail" if not net or not online else "not_verified", "network or app readiness not confirmed",
                          {"network": net, "platform": online})
        if any(part.get("kind") == "wifi_coprocessor" for part in self.primary.parts.get("network", [])):
            lines = session.send("AT+STATUS board")
            status = sdk_bench.records(lines, "+BOARD: coprocessor ")
            if len(status) != 1:
                return result("not_verified", "network online but coprocessor status was not reported")
            copro = status[0]
            if copro.get("stage") != "5" or copro.get("error") != "0":
                return result("fail", "coprocessor not ready", {"network": True, "platform": True})
        return result("pass", "network address and application platform online")

    def serial(self, endpoint):
        return result("pass", "running board, chip and full ELF matched AT and the generated vocabulary")

    def screen(self, endpoint):
        displays = self.primary.parts.get("display", [])
        if not displays:
            return result("not_verified", "headless interaction requires a light or sound witness")
        session = endpoint.session
        if not session.has("AT+SCREEN") or not session.has("AT+UI"):
            return result("not_verified", "this build does not advertise diagnostic screenshots")
        view = sdk_bench.one(session, "AT+UI", "+UI: ")
        width, height, pixels = self.kit.decode_lines(session.send("AT+SCREEN", wait=10), sdk_only=True)
        (self.out / "screen.png").write_bytes(self.kit.screen_png(width, height, pixels))
        matches = view.get("ready") == "1" and (view.get("width"), view.get("height")) == (str(width), str(height))
        visible = len({pixels[at:at + 3] for at in range(0, len(pixels), 3)}) > 1
        return result("not_verified" if matches and visible else "fail",
                      "non-blank screenshot saved for content review" if matches and visible else "screenshot is blank or does not match the active display",
                      {"file": "screen.png", "width": width, "height": height, "nonblank": visible})

    def memory_stack(self, endpoint):
        session = endpoint.session
        stack = app_stacks.read(session, self.tasks, 1024)
        heap = {}
        if session.has("AT+MEM"):
            for row in sdk_bench.records(session.send("AT+MEM"), "+MEM: "):
                if row.get("kind") in ("internal", "bulk", "dma"):
                    heap[row["kind"]] = {key: sdk_bench.unsigned(row, key) for key in ("free", "min_free", "largest")}
        workers = app_stacks.read_workers(session, 1024)
        below = stack["verdict"] == "fail" or any(row["status"] == "below_threshold" for row in workers.values())
        detail = ("observed task stack below 1024 bytes" if below else
                  "stack snapshot only; full application census and 30-minute idle not run")
        return result("fail" if below else "not_verified", detail,
                      {"stack": stack, "worker_watermarks": workers, "heap": heap})

    def one(self, endpoint, name):
        if name in {"ai", "room"}:
            from importlib import import_module

            module = import_module("gate_" + name)
            required = ("AT+AI START", "AT+AI STOP") if name == "ai" else (
                "AT+ROOM OPEN", "AT+ROOM PTT 1", "AT+ROOM PTT 0", "AT+ROOM CLOSE")
            require_permits(endpoint.session, required)
            self.ready(endpoint, self.primary)
            return module.run_one(endpoint.port, endpoint.session, endpoint.witness, endpoint.confirm)
        if name == "camera":
            from camera_sequence import Sequence, STEPS, VISION, summarize

            if not self.primary.parts.get("camera"):
                return result("not_verified", "product has no camera")
            steps = ["probe", "h5"]
            if endpoint.session.has(VISION) and set(STEPS["ai-vision"]) <= endpoint.session.permits:
                steps.insert(1, "ai-vision")
            if not any(set(STEPS[step]) <= endpoint.session.permits for step in steps if step != "h5"):
                raise Unverified("local camera action or required cleanup not authorized")
            self.ready(endpoint, self.primary)
            runner = Sequence(endpoint.port, endpoint.session, endpoint.witness, endpoint.confirm)
            evidence = summarize(steps, 1, runner)
            return result("fail" if evidence["verdict"] == "fail" else "not_verified",
                          "local camera diagnostics recorded; no confirmed phone live-view session", evidence)
        return getattr(self, name)(endpoint)

    def pair(self):
        if not self.peer:
            return result("not_verified", "no explicitly selected peer")
        from gate_pair import run_pair

        with self.open(self.primary) as left, self.open(self.peer) as right:
            self.ready(left, self.primary)
            self.ready(right, self.peer)
            return run_pair(left, right)

    def wechat(self):
        if not self.phone:
            return result("not_verified", "phone actions were not requested")
        if PHONE_ANSWER not in self.phone.permits:
            return result("not_verified", "exact phone answer action was not authorized")
        runner = getattr(self.kit, "gate_wx_out", None)
        if runner is None or not self.primary.info.get("device_id") or not self.primary.info.get("phone_wx_out"):
            return result("not_verified", "no verified outgoing phone profile for this device")
        with self.open(self.primary) as endpoint:
            try:
                index = sdk_bench.wechat_contact(endpoint.session)
            except ConsoleError as error:
                raise Unverified("no verified WeChat contact in the device cache") from error
            require_permits(endpoint.session, (f"AT+CALL INDEX {index}", "AT+CALL HANGUP"))
            self.ready(endpoint, self.primary)

        def confirm(action):
            self.confirm(self.primary)
            if action.startswith("PHONE_"):
                return action in self.phone.permits and action == PHONE_ANSWER
            return action in self.primary.permits

        log, outgoing = runner(self.primary, self.phone.serial, confirm)
        facts = {"outgoing": {key: outgoing[key] for key in ("gate_started", "pass", "phone_banner_verified",
                 "phone_answer_tapped", "device_connected", "phone_connected_duration_s", "media", "failure",
                 "cleanup_errors") if key in outgoing}, "outgoing_log": str(log / "summary.json"),
                 "incoming": "not_verified"}
        seconds = outgoing.get("phone_connected_duration_s")
        confirmed = (outgoing.get("pass") is True and outgoing.get("phone_banner_verified") is True
                     and outgoing.get("device_connected") is True and type(seconds) is int and seconds > 0)
        if confirmed:
            return result("not_verified", "outgoing phone call verified; incoming phone targeting is not configured", facts)
        return result("fail" if outgoing.get("gate_started") else "not_verified",
                      "outgoing phone call did not produce complete evidence", facts)

    def run(self):
        rows = []
        for name, label in ITEMS:
            try:
                if name in ACTIVE_ITEMS and not self.isolated:
                    check = result("not_verified", "active checks require explicit isolated_bench=true")
                elif name in {"boot", "wechat"}:
                    check = getattr(self, name)()
                elif name == "device_call":
                    check = self.pair()
                else:
                    with self.open(self.primary) as endpoint:
                        check = self.one(endpoint, name)
            except Unverified as error:
                check = result("not_verified", str(error))
            except (ConsoleError, OSError, TimeoutError) as error:
                check = result("fail", str(error))
            log = f"{name}.json"
            (self.out / log).write_text(json.dumps(check, ensure_ascii=False, indent=2), encoding="utf-8")
            rows.append({"item": name, "label": label, "log": log, **check})
            self.write_report(rows)
        return self.summary(rows)

    def summary(self, rows):
        statuses = {row["status"] for row in rows}
        complete = len(rows) == len(ITEMS)
        verdict = "fail" if "fail" in statuses else "pass" if complete and statuses == {"pass"} else "not_verified"
        return {"schema": 1, "product": self.primary.product, "elf_sha256": self.primary.sha,
                "isolated_bench": self.isolated,
                "peer_product": self.peer.product if self.peer else None,
                "peer_elf_sha256": self.peer.sha if self.peer else None, "complete": complete,
                "verdict": verdict, "items": rows}

    def write_report(self, rows):
        (self.out / "summary.json").write_text(json.dumps(self.summary(rows), ensure_ascii=False, indent=2), encoding="utf-8")
        lines = [f"# 基础功能关：{self.primary.product}", "", f"ELF SHA-256：`{self.primary.sha}`", "",
                 "| 项目 | 结果 | 证据 |", "| --- | --- | --- |"]
        for row in rows:
            detail = " ".join(str(row["detail"]).split()).replace("|", "\\|")
            lines.append(f"| {row['label']} | {row['status']} | [{detail}]({row['log']}) |")
        (self.out / f"基础功能关-{self.primary.product}.md").write_text("\n".join(lines) + "\n", encoding="utf-8")


def require_permits(session, commands):
    for action in commands:
        if action not in session.permits or not session.has(action) or session.check(action).access != "action":
            raise Unverified("required exact command or cleanup permission is absent")
